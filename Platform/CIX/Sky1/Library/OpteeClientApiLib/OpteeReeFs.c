/** @file
  In-memory OP-TEE REE FS for firmware.

  OP-TEE asks tee-supplicant to store REE FS files ("dirf.db" and
  "<hex file number>") for TEE_STORAGE_PRIVATE objects. On Sky1 the fTPM TA
  keeps its NV in OP-TEE's secure NOR storage (TEE_STORAGE_PRIVATE_NOR), and
  the only REE FS user is OP-TEE's TA version database (ta_ver.db), written
  while a TA is loaded. That database lives in normal-world storage and gives
  no protection against an attacker who can rewrite firmware, which is where
  the TA comes from, so firmware keeps the files in memory only. OP-TEE closes
  them before the TA load completes; nothing here outlives boot services.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>

#include "OpteeClientInternal.h"

#define REE_FS_NAME_MAX   32
#define REE_FS_MAX_FILES  16

typedef struct {
  CHAR8      Name[REE_FS_NAME_MAX];
  BOOLEAN    InUse;
  UINT8      *Data;
  UINTN      Size;
  UINTN      Capacity;
} REE_FS_FILE;

STATIC REE_FS_FILE  mFiles[REE_FS_MAX_FILES];

//
// Descriptors are slot numbers plus one; a file keeps its slot until removed.
//
STATIC
UINT64
FileFd (
  IN REE_FS_FILE  *File
  )
{
  return (UINT64)(File - mFiles) + 1;
}

STATIC
REE_FS_FILE *
FindByFd (
  IN UINT64  Fd
  )
{
  if ((Fd == 0) || (Fd > REE_FS_MAX_FILES) || !mFiles[Fd - 1].InUse) {
    return NULL;
  }

  return &mFiles[Fd - 1];
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

STATIC
VOID
Clear (
  IN REE_FS_FILE  *File
  )
{
  if (File->Data != NULL) {
    FreePool (File->Data);
  }

  ZeroMem (File, sizeof (*File));
}

/**
  Resize the file; new bytes read as zero.
**/
STATIC
BOOLEAN
SetSize (
  IN OUT REE_FS_FILE  *File,
  IN     UINTN        Size
  )
{
  UINTN  Capacity;
  UINT8  *Data;

  if (Size > File->Capacity) {
    Capacity = ALIGN_VALUE (Size, SIZE_4KB);
    Data     = ReallocatePool (File->Capacity, Capacity, File->Data);
    if (Data == NULL) {
      return FALSE;
    }

    ZeroMem (Data + File->Capacity, Capacity - File->Capacity);
    File->Data     = Data;
    File->Capacity = Capacity;
  } else if (Size < File->Size) {
    ZeroMem (File->Data + Size, File->Size - Size);
  }

  File->Size = Size;
  return TRUE;
}

/**
  Copy the NUL-terminated path from a memref parameter.
**/
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

  if (!OpteeMsgParamToBuffer (Param, &Buffer, &Size) || (Buffer == NULL)) {
    return FALSE;
  }

  Length = AsciiStrnLenS (Buffer, Size);
  if ((Length == 0) || (Length == Size) || (Length >= REE_FS_NAME_MAX)) {
    return FALSE;
  }

  CopyMem (Name, Buffer, Length + 1);
  return TRUE;
}

/**
  OPEN/CREATE: [in] value[0].a op, [in] memref[1] path, [out] value[2].a fd
**/
STATIC
TEEC_Result
FsOpen (
  IN OUT OPTEE_MSG_ARG  *Arg,
  IN     BOOLEAN        Create
  )
{
  CHAR8        Name[REE_FS_NAME_MAX];
  REE_FS_FILE  *File;
  UINTN        Index;

  if ((Arg->NumParams != 3) || !GetName (&Arg->Params[1], Name)) {
    return TEEC_ERROR_BAD_PARAMETERS;
  }

  File = FindByName (Name);
  if (!Create) {
    if (File == NULL) {
      return TEEC_ERROR_ITEM_NOT_FOUND;
    }
  } else if (File != NULL) {
    //
    // O_CREAT | O_TRUNC
    //
    SetSize (File, 0);
  } else {
    for (Index = 0; Index < REE_FS_MAX_FILES && mFiles[Index].InUse; Index++) {
    }

    if (Index == REE_FS_MAX_FILES) {
      return TEEC_ERROR_OUT_OF_MEMORY;
    }

    File        = &mFiles[Index];
    File->InUse = TRUE;
    AsciiStrCpyS (File->Name, REE_FS_NAME_MAX, Name);
  }

  Arg->Params[2].U.Value.A = FileFd (File);
  return TEEC_SUCCESS;
}

/**
  READ:  [in] value[0].b fd, value[0].c offset, [out] memref[1]
  WRITE: [in] value[0].b fd, value[0].c offset, [in]  memref[1]
**/
STATIC
TEEC_Result
FsReadWrite (
  IN OUT OPTEE_MSG_ARG  *Arg,
  IN     BOOLEAN        Write
  )
{
  REE_FS_FILE  *File;
  VOID         *Buffer;
  UINTN        Length;
  UINT64       Offset;
  UINTN        Copy;

  if ((Arg->NumParams != 2) || !OpteeMsgParamToBuffer (&Arg->Params[1], &Buffer, &Length) ||
      ((Buffer == NULL) && (Length != 0)))
  {
    return TEEC_ERROR_BAD_PARAMETERS;
  }

  File   = FindByFd (Arg->Params[0].U.Value.B);
  Offset = Arg->Params[0].U.Value.C;
  if (File == NULL) {
    return TEEC_ERROR_GENERIC;
  }

  if (!Write) {
    Copy = (Offset >= File->Size) ? 0 : MIN (Length, File->Size - (UINTN)Offset);
    CopyMem (Buffer, File->Data + Offset, Copy);
    OpteeMsgParamSetSize (&Arg->Params[1], Copy);
    return TEEC_SUCCESS;
  }

  if (Length == 0) {
    return TEEC_SUCCESS;
  }

  if (Offset > MAX_UINTN - Length) {
    return TEEC_ERROR_BAD_PARAMETERS;
  }

  if (((UINTN)Offset + Length > File->Size) && !SetSize (File, (UINTN)Offset + Length)) {
    return TEEC_ERROR_OUT_OF_MEMORY;
  }

  CopyMem (File->Data + Offset, Buffer, Length);
  return TEEC_SUCCESS;
}

/**
  RENAME: [in] value[0].b overwrite, [in] memref[1] old path, [in] memref[2] new path
**/
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
  CHAR8        Name[REE_FS_NAME_MAX];
  REE_FS_FILE  *File;

  if ((Arg->NumParams == 0) ||
      ((Arg->Params[0].Attr & OPTEE_MSG_ATTR_TYPE_MASK) != OPTEE_MSG_ATTR_TYPE_VALUE_INPUT))
  {
    return TEEC_ERROR_BAD_PARAMETERS;
  }

  switch (Arg->Params[0].U.Value.A) {
    case OPTEE_RPC_FS_OPEN:
      return FsOpen (Arg, FALSE);

    case OPTEE_RPC_FS_CREATE:
      return FsOpen (Arg, TRUE);

    case OPTEE_RPC_FS_CLOSE:
      //
      // [in] value[0].b fd. Files stay in memory until removed.
      //
      return (FindByFd (Arg->Params[0].U.Value.B) != NULL) ? TEEC_SUCCESS : TEEC_ERROR_GENERIC;

    case OPTEE_RPC_FS_READ:
      return FsReadWrite (Arg, FALSE);

    case OPTEE_RPC_FS_WRITE:
      return FsReadWrite (Arg, TRUE);

    case OPTEE_RPC_FS_TRUNCATE:
      //
      // [in] value[0].b fd, value[0].c length
      //
      File = FindByFd (Arg->Params[0].U.Value.B);
      if (File == NULL) {
        return TEEC_ERROR_GENERIC;
      }

      return SetSize (File, (UINTN)Arg->Params[0].U.Value.C) ? TEEC_SUCCESS : TEEC_ERROR_OUT_OF_MEMORY;

    case OPTEE_RPC_FS_REMOVE:
      //
      // [in] memref[1] path
      //
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
      //
      // REE FS enumerates objects through dirf.db, never through directories.
      //
      return TEEC_ERROR_ITEM_NOT_FOUND;

    default:
      return TEEC_ERROR_NOT_SUPPORTED;
  }
}
