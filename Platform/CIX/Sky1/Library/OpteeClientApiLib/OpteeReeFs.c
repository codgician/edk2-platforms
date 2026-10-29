/** @file
  Bounded, volatile OP-TEE REE filesystem for the TA version database.

  The fTPM NV uses secure NOR, not this filesystem. Files here survive CLOSE
  but not client finalization. They provide no persistent anti-rollback floor.
  Unlink and rename-overwrite deliberately invalidate all descriptors to the
  removed file. This conservative policy releases and scrubs storage at once;
  unlike slot-based descriptors, a stale ID can never name a replacement.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>

#include "OpteeClientInternal.h"

#define REE_FS_NAME_MAX         32
#define REE_FS_MAX_FILES        16
#define REE_FS_MAX_DESCRIPTORS  64
#define REE_FS_FILE_CAPACITY    (1024U * 1024U)
#define REE_FS_TOTAL_CAPACITY   (4U * REE_FS_FILE_CAPACITY)

typedef struct {
  CHAR8      Name[REE_FS_NAME_MAX];
  BOOLEAN    InUse;
  UINT8      *Data;
  UINTN      Size;
  UINTN      Capacity;
} REE_FS_FILE;

typedef struct {
  UINT32         Id;
  REE_FS_FILE    *File;
} REE_FS_DESCRIPTOR;

STATIC REE_FS_FILE        mFiles[REE_FS_MAX_FILES];
STATIC REE_FS_DESCRIPTOR  mDescriptors[REE_FS_MAX_DESCRIPTORS];
STATIC UINTN             mTotalCapacity;
// Never reset this counter: even a finalized client's descriptors stay stale.
STATIC UINT32            mLastDescriptorId;

STATIC
REE_FS_DESCRIPTOR *
FindDescriptor (
  IN UINT64  Id
  )
{
  UINTN  Index;

  if ((Id == 0) || (Id > MAX_INT32)) {
    return NULL;
  }

  for (Index = 0; Index < REE_FS_MAX_DESCRIPTORS; Index++) {
    if ((mDescriptors[Index].File != NULL) && (mDescriptors[Index].Id == Id)) {
      return &mDescriptors[Index];
    }
  }

  return NULL;
}

STATIC
REE_FS_FILE *
FindByName (
  IN CONST CHAR8  *Name
  )
{
  UINTN  Index;

  for (Index = 0; Index < REE_FS_MAX_FILES; Index++) {
    if (mFiles[Index].InUse && (AsciiStrCmp (mFiles[Index].Name, Name) == 0)) {
      return &mFiles[Index];
    }
  }

  return NULL;
}

/** Scrub all allocated bytes and revoke descriptors before reusing a slot. */
STATIC
VOID
Clear (
  IN REE_FS_FILE  *File
  )
{
  UINTN  Index;

  for (Index = 0; Index < REE_FS_MAX_DESCRIPTORS; Index++) {
    if (mDescriptors[Index].File == File) {
      ZeroMem (&mDescriptors[Index], sizeof (mDescriptors[Index]));
    }
  }

  if (File->Data != NULL) {
    ZeroMem (File->Data, File->Capacity);
    FreePool (File->Data);
  }

  mTotalCapacity -= File->Capacity;
  ZeroMem (File, sizeof (*File));
}

/** Reset all volatile state except the never-reused descriptor ID counter. */
VOID
OpteeReeFsFinalize (
  VOID
  )
{
  UINTN  Index;

  for (Index = 0; Index < REE_FS_MAX_FILES; Index++) {
    Clear (&mFiles[Index]);
  }

  ZeroMem (mDescriptors, sizeof (mDescriptors));
}

/** Resize atomically; capacity is page-rounded and all new bytes are zero. */
STATIC
BOOLEAN
SetSize (
  IN OUT REE_FS_FILE  *File,
  IN     UINT64       RequestedSize
  )
{
  UINTN  Size;
  UINTN  Capacity;
  UINT8  *Data;

  if ((RequestedSize > REE_FS_FILE_CAPACITY) ||
      (RequestedSize > MAX_UINTN - (SIZE_4KB - 1)))
  {
    return FALSE;
  }

  Size = (UINTN)RequestedSize;
  if (Size > File->Capacity) {
    Capacity = (Size + SIZE_4KB - 1) & ~(UINTN)(SIZE_4KB - 1);
    if (Capacity - File->Capacity > REE_FS_TOTAL_CAPACITY - mTotalCapacity) {
      return FALSE;
    }

    // ReallocatePool would release the old bytes without scrubbing them.
    Data = AllocateZeroPool (Capacity);
    if (Data == NULL) {
      return FALSE;
    }

    if (File->Data != NULL) {
      CopyMem (Data, File->Data, File->Size);
      ZeroMem (File->Data, File->Capacity);
      FreePool (File->Data);
    }

    mTotalCapacity += Capacity - File->Capacity;
    File->Data     = Data;
    File->Capacity = Capacity;
  } else if (Size < File->Size) {
    ZeroMem (File->Data + Size, File->Size - Size);
  } else if (Size > File->Size) {
    ZeroMem (File->Data + File->Size, Size - File->Size);
  }

  File->Size = Size;
  return TRUE;
}

/** Paths are input-only memrefs containing 1..31 bytes followed by a NUL. */
STATIC
BOOLEAN
GetName (
  IN  OPTEE_MSG_PARAM  *Param,
  OUT CHAR8            *Name
  )
{
  VOID   *Buffer;
  UINTN  Size;
  UINTN  Length;

  if ((Param->Attr != OPTEE_MSG_ATTR_TYPE_TMEM_INPUT) ||
      !OpteeMsgParamToBuffer (Param, &Buffer, &Size) || (Buffer == NULL))
  {
    return FALSE;
  }

  // Validate a private path snapshot: a peer changing its terminator after
  // validation must not turn later string operations into unbounded reads.
  Size = MIN (Size, REE_FS_NAME_MAX);
  CopyMem (Name, Buffer, Size);
  Length = AsciiStrnLenS (Name, Size);
  if ((Length == 0) || (Length >= Size)) {
    return FALSE;
  }

  return TRUE;
}

/** OPEN/CREATE: IN value, IN path, OUT descriptor. */
STATIC
TEEC_Result
FsOpen (
  IN OUT OPTEE_MSG_ARG  *Arg,
  IN     BOOLEAN        Create
  )
{
  CHAR8                Name[REE_FS_NAME_MAX];
  REE_FS_FILE          *File;
  REE_FS_DESCRIPTOR    *Descriptor;
  UINTN                Index;

  if ((Arg->NumParams != 3) ||
      (Arg->Params[2].Attr != OPTEE_MSG_ATTR_TYPE_VALUE_OUTPUT) ||
      !GetName (&Arg->Params[1], Name))
  {
    return TEEC_ERROR_BAD_PARAMETERS;
  }

  File = FindByName (Name);
  if (!Create && (File == NULL)) {
    return TEEC_ERROR_ITEM_NOT_FOUND;
  }

  // Reserve every resource before CREATE can truncate an existing file.
  if (mLastDescriptorId == MAX_INT32) {
    return TEEC_ERROR_OUT_OF_MEMORY;
  }

  for (Index = 0; Index < REE_FS_MAX_DESCRIPTORS && (mDescriptors[Index].File != NULL); Index++) {
  }

  if (Index == REE_FS_MAX_DESCRIPTORS) {
    return TEEC_ERROR_OUT_OF_MEMORY;
  }

  Descriptor = &mDescriptors[Index];
  if (File == NULL) {
    for (Index = 0; Index < REE_FS_MAX_FILES && mFiles[Index].InUse; Index++) {
    }

    if (Index == REE_FS_MAX_FILES) {
      return TEEC_ERROR_OUT_OF_MEMORY;
    }

    File        = &mFiles[Index];
    File->InUse = TRUE;
    AsciiStrCpyS (File->Name, REE_FS_NAME_MAX, Name);
  } else if (Create) {
    SetSize (File, 0);
  }

  Descriptor->File         = File;
  Descriptor->Id           = ++mLastDescriptorId;
  Arg->Params[2].U.Value.A = Descriptor->Id;
  return TEEC_SUCCESS;
}

/** READ takes an output memref; WRITE takes an input memref. */
STATIC
TEEC_Result
FsReadWrite (
  IN OUT OPTEE_MSG_ARG  *Arg,
  IN     BOOLEAN        Write
  )
{
  REE_FS_DESCRIPTOR  *Descriptor;
  REE_FS_FILE        *File;
  VOID               *Buffer;
  UINTN              Length;
  UINT64             Offset;
  UINTN              Copy;

  if ((Arg->NumParams != 2) ||
      (Arg->Params[1].Attr != (Write ? OPTEE_MSG_ATTR_TYPE_TMEM_INPUT : OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT)) ||
      !OpteeMsgParamToBuffer (&Arg->Params[1], &Buffer, &Length))
  {
    return TEEC_ERROR_BAD_PARAMETERS;
  }

  Descriptor = FindDescriptor (Arg->Params[0].U.Value.B);
  Offset     = Arg->Params[0].U.Value.C;
  if (Descriptor == NULL) {
    return TEEC_ERROR_GENERIC;
  }

  File = Descriptor->File;
  if (!Write) {
    // Do not even form an out-of-bounds pointer for EOF or an empty read.
    Copy = (Offset >= File->Size) ? 0 : MIN (Length, File->Size - (UINTN)Offset);
    if (Copy != 0) {
      CopyMem (Buffer, File->Data + (UINTN)Offset, Copy);
    }

    OpteeMsgParamSetSize (&Arg->Params[1], Copy);
    return TEEC_SUCCESS;
  }

  if (Length == 0) {
    return TEEC_SUCCESS;
  }

  if (Offset > MAX_UINTN - Length) {
    return TEEC_ERROR_BAD_PARAMETERS;
  }

  if (((UINTN)Offset + Length > File->Size) && !SetSize (File, Offset + Length)) {
    return TEEC_ERROR_OUT_OF_MEMORY;
  }

  CopyMem (File->Data + (UINTN)Offset, Buffer, Length);
  return TEEC_SUCCESS;
}

/** Rename preserves source descriptors and revokes overwritten target ones. */
STATIC
TEEC_Result
FsRename (
  IN OUT OPTEE_MSG_ARG  *Arg
  )
{
  CHAR8        OldName[REE_FS_NAME_MAX];
  CHAR8        NewName[REE_FS_NAME_MAX];
  REE_FS_FILE  *Old;
  REE_FS_FILE  *New;

  if ((Arg->NumParams != 3) || !GetName (&Arg->Params[1], OldName) || !GetName (&Arg->Params[2], NewName)) {
    return TEEC_ERROR_BAD_PARAMETERS;
  }

  Old = FindByName (OldName);
  if (Old == NULL) {
    return TEEC_ERROR_ITEM_NOT_FOUND;
  }

  New = FindByName (NewName);
  if ((New != NULL) && (New != Old)) {
    if (Arg->Params[0].U.Value.B == 0) {
      return TEEC_ERROR_ACCESS_CONFLICT;
    }

    Clear (New);
  }

  AsciiStrCpyS (Old->Name, REE_FS_NAME_MAX, NewName);
  return TEEC_SUCCESS;
}

TEEC_Result
OpteeReeFsHandle (
  IN OUT OPTEE_MSG_ARG  *Arg
  )
{
  CHAR8                Name[REE_FS_NAME_MAX];
  REE_FS_FILE          *File;
  REE_FS_DESCRIPTOR    *Descriptor;

  if ((Arg == NULL) || (Arg->NumParams == 0) ||
      (Arg->Params[0].Attr != OPTEE_MSG_ATTR_TYPE_VALUE_INPUT))
  {
    return TEEC_ERROR_BAD_PARAMETERS;
  }

  switch (Arg->Params[0].U.Value.A) {
    case OPTEE_RPC_FS_OPEN:
      return FsOpen (Arg, FALSE);
    case OPTEE_RPC_FS_CREATE:
      return FsOpen (Arg, TRUE);
    case OPTEE_RPC_FS_CLOSE:
      if (Arg->NumParams != 1) {
        return TEEC_ERROR_BAD_PARAMETERS;
      }

      Descriptor = FindDescriptor (Arg->Params[0].U.Value.B);
      if (Descriptor == NULL) {
        return TEEC_ERROR_GENERIC;
      }

      ZeroMem (Descriptor, sizeof (*Descriptor));
      return TEEC_SUCCESS;
    case OPTEE_RPC_FS_READ:
      return FsReadWrite (Arg, FALSE);
    case OPTEE_RPC_FS_WRITE:
      return FsReadWrite (Arg, TRUE);
    case OPTEE_RPC_FS_TRUNCATE:
      if (Arg->NumParams != 1) {
        return TEEC_ERROR_BAD_PARAMETERS;
      }

      Descriptor = FindDescriptor (Arg->Params[0].U.Value.B);
      if (Descriptor == NULL) {
        return TEEC_ERROR_GENERIC;
      }

      return SetSize (Descriptor->File, Arg->Params[0].U.Value.C) ? TEEC_SUCCESS : TEEC_ERROR_OUT_OF_MEMORY;
    case OPTEE_RPC_FS_REMOVE:
      if ((Arg->NumParams != 2) || !GetName (&Arg->Params[1], Name)) {
        return TEEC_ERROR_BAD_PARAMETERS;
      }

      File = FindByName (Name);
      if (File == NULL) {
        return TEEC_ERROR_ITEM_NOT_FOUND;
      }

      Clear (File);
      return TEEC_SUCCESS;
    case OPTEE_RPC_FS_RENAME:
      return FsRename (Arg);
    case OPTEE_RPC_FS_OPENDIR:
      if ((Arg->NumParams != 3) ||
          (Arg->Params[2].Attr != OPTEE_MSG_ATTR_TYPE_VALUE_OUTPUT) ||
          !GetName (&Arg->Params[1], Name))
      {
        return TEEC_ERROR_BAD_PARAMETERS;
      }

      // Object enumeration uses dirf.db; directory support is unnecessary.
      return TEEC_ERROR_ITEM_NOT_FOUND;
    default:
      return TEEC_ERROR_NOT_SUPPORTED;
  }
}
