/** @file
  GlobalPlatform client over the CIX OP-TEE reserved-memory SMC interface.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Library/ArmMmuLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/OpteeClientApiLib.h>
#include <Library/PcdLib.h>
#include <Library/TimerLib.h>
#include "OpteeClientInternal.h"
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>

STATIC BOOLEAN  mSerialOnly;

VOID
OpteeClientApiExitBootServices (
  VOID
  )
{
  mSerialOnly = TRUE;
}

VOID
OpteeReject (
  IN CONST CHAR8  *Check,
  IN UINT64       Value,
  IN UINT64       Status
  )
{
  CHAR8   Line[192];
  CHAR16  ConsoleLine[192];
  UINTN   Length;
  UINTN   Index;

  Length = AsciiSPrint (Line, sizeof (Line),
                       "TPM OpteeClient: %a value=0x%lx (0x%lx)\r\n",
                       Check, Value, Status);
  SerialPortWrite ((UINT8 *)Line, Length);
  if (!mSerialOnly && (gST != NULL) && (gST->ConOut != NULL) &&
      (gST->ConOut->OutputString != NULL))
  {
    for (Index = 0; Index <= Length; Index++) {
      ConsoleLine[Index] = (CHAR16)Line[Index];
    }

    gST->ConOut->OutputString (gST->ConOut, ConsoleLine);
  }
}

#define SHM_PAGE_SIZE  SIZE_4KB

typedef struct {
  UINTN     Pages;
  UINTN     Size;
  UINT32    Owner;
} SHM_RECORD;

STATIC BOOLEAN     mInitialized;
STATIC BOOLEAN     mBroken;
STATIC BOOLEAN     mInCall;
STATIC UINT64      mShmBase;
STATIC UINTN       mShmPages;
STATIC SHM_RECORD  *mShm;
STATIC UINT64      mDelayMs;

/** Find an exact live allocation base, not an interior page. */
STATIC
SHM_RECORD *
ShmRecord (
  IN UINT64  Address
  )
{
  UINT64  Offset;

  if ((mShm == NULL) || (Address < mShmBase)) {
    return NULL;
  }

  Offset = Address - mShmBase;
  if ((Offset / SHM_PAGE_SIZE >= mShmPages) || (Offset % SHM_PAGE_SIZE != 0) ||
      (mShm[Offset / SHM_PAGE_SIZE].Pages == 0))
  {
    return NULL;
  }

  return &mShm[Offset / SHM_PAGE_SIZE];
}

EFI_STATUS
OpteeShmInit (
  VOID
  )
{
  ARM_SMC_ARGS  Args;
  UINT64        Base;
  UINT64        Size;
  UINT64        ReservedBase;
  UINT64        ReservedSize;
  UINTN         Pages;
  SHM_RECORD    *Records;
  EFI_STATUS    Status;

  if (mShm != NULL) {
    return EFI_ALREADY_STARTED;
  }

  ZeroMem (&Args, sizeof (Args));
  Args.Arg0 = OPTEE_SMC_EXCHANGE_CAPABILITIES;
  ArmCallSmc (&Args);
  if ((Args.Arg0 != OPTEE_SMC_RETURN_OK) ||
      ((Args.Arg1 & OPTEE_SMC_SEC_CAP_HAVE_RESERVED_SHM) == 0))
  {
    OpteeReject ("SHM capabilities", (Args.Arg0 != OPTEE_SMC_RETURN_OK) ? Args.Arg0 : Args.Arg1, EFI_UNSUPPORTED);
    return EFI_UNSUPPORTED;
  }

  ZeroMem (&Args, sizeof (Args));
  Args.Arg0 = OPTEE_SMC_GET_SHM_CONFIG;
  ArmCallSmc (&Args);
  Base         = Args.Arg1;
  Size         = Args.Arg2;
  ReservedBase = FixedPcdGet32 (PcdTrustZoneSharedMemoryBase);
  ReservedSize = FixedPcdGet32 (PcdTrustZoneSharedMemorySize);
  // The trusted platform reservation, not the peer, determines what may map.
  if ((Args.Arg0 != OPTEE_SMC_RETURN_OK) || (Base == 0) ||
      (Base % SHM_PAGE_SIZE != 0) || (Size == 0) ||
      (Size % SHM_PAGE_SIZE != 0) || (Base > MAX_UINTN) ||
      (Size > MAX_UINTN - Base) || (Base < ReservedBase) ||
      (Base - ReservedBase > ReservedSize) ||
      (Size > ReservedSize - (Base - ReservedBase)) ||
      ((Args.Arg3 != 0) && (Args.Arg3 != OPTEE_SMC_SHM_CACHED)))
  {
    if (Args.Arg0 != OPTEE_SMC_RETURN_OK) {
      OpteeReject ("SHM configuration status", Args.Arg0, EFI_UNSUPPORTED);
    } else if ((Args.Arg3 != 0) && (Args.Arg3 != OPTEE_SMC_SHM_CACHED)) {
      OpteeReject ("SHM cache mode", Args.Arg3, EFI_UNSUPPORTED);
    } else if ((Base == 0) || (Base % SHM_PAGE_SIZE != 0) ||
               (Base > MAX_UINTN) || (Base < ReservedBase) ||
               (Base - ReservedBase > ReservedSize))
    {
      OpteeReject ("SHM base", Base, EFI_UNSUPPORTED);
    } else {
      OpteeReject ("SHM size", Size, EFI_UNSUPPORTED);
    }
    return EFI_UNSUPPORTED;
  }

  Pages = (UINTN)(Size / SHM_PAGE_SIZE);
  if (Pages > MAX_UINTN / sizeof (*Records)) {
    OpteeReject ("SHM record count", Pages, EFI_UNSUPPORTED);
    return EFI_UNSUPPORTED;
  }

  Records = AllocateZeroPool (Pages * sizeof (*Records));
  if (Records == NULL) {
    OpteeReject ("SHM records allocation", Pages, EFI_OUT_OF_RESOURCES);
    return EFI_OUT_OF_RESOURCES;
  }

  Status = ArmSetMemoryAttributes (
             Base,
             Size,
             (Args.Arg3 == OPTEE_SMC_SHM_CACHED) ? EFI_MEMORY_WB : EFI_MEMORY_UC,
             0
             );
  if (EFI_ERROR (Status)) {
    OpteeReject ("SHM mapping", Size, Status);
    FreePool (Records);
    return Status;
  }

  mShmBase  = Base;
  mShmPages = Pages;
  mShm      = Records;
  return EFI_SUCCESS;
}

VOID *
OpteeShmAllocOwned (
  IN UINTN   Size,
  IN UINTN   Alignment,
  IN UINT32  Owner
  )
{
  UINTN   Pages;
  UINTN   Start;
  UINTN   Index;
  UINT64  Address;

  if (!mInitialized || mBroken || (Owner < OPTEE_SHM_CLIENT) ||
      (Owner > OPTEE_SHM_RPC_GLOBAL) ||
      (Size > mShmPages * SHM_PAGE_SIZE) ||
      (Alignment == 0) || ((Alignment & (Alignment - 1)) != 0) ||
      (Alignment > mShmPages * SHM_PAGE_SIZE))
  {
    return NULL;
  }

  Pages = MAX (Size / SHM_PAGE_SIZE + ((Size % SHM_PAGE_SIZE != 0) ? 1 : 0), 1);
  for (Start = 0; Start <= mShmPages - Pages; Start++) {
    Address = mShmBase + Start * SHM_PAGE_SIZE;
    if ((Address & (Alignment - 1)) != 0) {
      continue;
    }

    for (Index = 0; Index < Pages; Index++) {
      if (mShm[Start + Index].Owner != 0) {
        break;
      }
    }

    if (Index != Pages) {
      Start += Index;
      continue;
    }

    for (Index = 0; Index < Pages; Index++) {
      mShm[Start + Index].Owner = Owner;
    }

    mShm[Start].Pages = Pages;
    mShm[Start].Size  = Size;
    ZeroMem ((VOID *)(UINTN)Address, Pages * SHM_PAGE_SIZE);
    return (VOID *)(UINTN)Address;
  }

  return NULL;
}

VOID *
OpteeShmAlloc (
  IN UINTN  Size,
  IN UINTN  Alignment
  )
{
  return OpteeShmAllocOwned (Size, Alignment, OPTEE_SHM_CLIENT);
}

BOOLEAN
OpteeShmFreeOwned (
  IN VOID    *Buffer,
  IN UINT32  Owner
  )
{
  SHM_RECORD  *Record;
  UINTN       Pages;

  Record = ShmRecord ((UINTN)Buffer);
  if ((Record == NULL) || (Record->Owner != Owner)) {
    return FALSE;
  }

  Pages = Record->Pages;
  ZeroMem (Buffer, Pages * SHM_PAGE_SIZE);
  ZeroMem (Record, Pages * sizeof (*Record));
  return TRUE;
}

VOID
OpteeShmFree (
  IN VOID  *Buffer
  )
{
  OpteeShmFreeOwned (Buffer, OPTEE_SHM_CLIENT);
}

BOOLEAN
OpteeShmIsOwned (
  IN UINT64  Address,
  IN UINT64  Size,
  IN UINT32  Owner
  )
{
  SHM_RECORD  *Record;

  Record = ShmRecord (Address);
  return (Record != NULL) && (Record->Owner == Owner) && (Size <= Record->Size);
}

BOOLEAN
OpteeShmContains (
  IN UINT64  Address,
  IN UINT64  Size
  )
{
  UINTN   Page;
  UINT64  Offset;

  if ((mShm == NULL) || (Address < mShmBase) ||
      ((Address - mShmBase) / SHM_PAGE_SIZE >= mShmPages))
  {
    return FALSE;
  }

  Page = (UINTN)((Address - mShmBase) / SHM_PAGE_SIZE);
  if (mShm[Page].Owner == 0) {
    return FALSE;
  }

  while ((Page > 0) && (mShm[Page].Pages == 0)) {
    Page--;
  }

  Offset = Address - (mShmBase + Page * SHM_PAGE_SIZE);
  return (mShm[Page].Pages != 0) && (Offset <= mShm[Page].Size) &&
         (Size <= mShm[Page].Size - Offset);
}

BOOLEAN
OpteeMsgParamToBuffer (
  IN  OPTEE_MSG_PARAM  *Param,
  OUT VOID             **Buffer,
  OUT UINTN            *Size
  )
{
  SHM_RECORD  *Record;
  UINT64      Address;
  UINT64      Length;
  UINT64      Offset;

  if ((Param->Attr != OPTEE_MSG_ATTR_TYPE_TMEM_INPUT) &&
      (Param->Attr != OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT) &&
      (Param->Attr != OPTEE_MSG_ATTR_TYPE_TMEM_INOUT))
  {
    return FALSE;
  }

  Address = Param->U.Tmem.BufPtr;
  Length  = Param->U.Tmem.Size;
  if (Address == 0) {
    if ((Length != 0) || (Param->U.Tmem.ShmRef != 0)) {
      return FALSE;
    }
  } else {
    Record = ShmRecord (Param->U.Tmem.ShmRef);
    if ((Record == NULL) || (Record->Owner < OPTEE_SHM_RPC_APPL) ||
        (Record->Owner > OPTEE_SHM_RPC_GLOBAL) || (Address < Param->U.Tmem.ShmRef))
    {
      return FALSE;
    }

    Offset = Address - Param->U.Tmem.ShmRef;
    if ((Offset > Record->Size) || (Length > Record->Size - Offset)) {
      return FALSE;
    }
  }

  *Buffer = (VOID *)(UINTN)Address;
  *Size   = (UINTN)Length;
  return TRUE;
}

UINT64
OpteeMsgParamRejectValue (
  IN CONST OPTEE_MSG_PARAM  *Param
  )
{
  SHM_RECORD  *Record;

  if (Param->U.Tmem.BufPtr == 0) {
    return (Param->U.Tmem.Size != 0) ? Param->U.Tmem.Size : Param->U.Tmem.ShmRef;
  }

  Record = ShmRecord (Param->U.Tmem.ShmRef);
  if ((Record == NULL) || (Record->Owner < OPTEE_SHM_RPC_APPL) ||
      (Record->Owner > OPTEE_SHM_RPC_GLOBAL))
  {
    return Param->U.Tmem.ShmRef;
  }

  if ((Param->U.Tmem.BufPtr < Param->U.Tmem.ShmRef) ||
      (Param->U.Tmem.BufPtr - Param->U.Tmem.ShmRef > Record->Size))
  {
    return Param->U.Tmem.BufPtr;
  }

  return Param->U.Tmem.Size;
}

VOID
OpteeMsgParamSetSize (
  IN OUT OPTEE_MSG_PARAM  *Param,
  IN     UINTN            Size
  )
{
  Param->U.Tmem.Size = Size;
}

STATIC
VOID
ReleaseRpcAllocations (
  VOID
  )
{
  UINTN  Index;

  for (Index = 0; Index < mShmPages; Index++) {
    if ((mShm[Index].Pages != 0) && (mShm[Index].Owner != OPTEE_SHM_CLIENT)) {
      OpteeShmFreeOwned ((VOID *)(UINTN)(mShmBase + Index * SHM_PAGE_SIZE), mShm[Index].Owner);
    }
  }
}

BOOLEAN
OpteeRpcDelay (
  IN UINT64  Milliseconds
  )
{
  if ((Milliseconds > 100) || (Milliseconds > 1000 - mDelayMs)) {
    // The RPC dispatcher reports this rejection once, including the delay.
    return FALSE;
  }

  mDelayMs += Milliseconds;
  MicroSecondDelay ((UINTN)Milliseconds * 1000);
  return TRUE;
}

TEEC_Result
OpteeCallWithArg (
  IN OPTEE_MSG_ARG  *Arg
  )
{
  ARM_SMC_ARGS  Args;
  UINT64        Address;
  UINTN         Count;
  TEEC_Result   Result;

  if (!mInitialized || mBroken || mInCall ||
      !OpteeShmIsOwned ((UINTN)Arg, sizeof (*Arg), OPTEE_SHM_CLIENT) ||
      (Arg->NumParams > TEEC_CONFIG_PAYLOAD_REF_COUNT + 2) ||
      !OpteeShmIsOwned ((UINTN)Arg, OPTEE_MSG_ARG_SIZE (Arg->NumParams), OPTEE_SHM_CLIENT))
  {
    return TEEC_ERROR_COMMUNICATION;
  }

  Address   = (UINTN)Arg;
  mInCall   = TRUE;
  mDelayMs  = 0;
  Arg->Ret  = TEEC_ERROR_COMMUNICATION;
  ZeroMem (&Args, sizeof (Args));
  Args.Arg0 = OPTEE_SMC_CALL_WITH_ARG;
  Args.Arg1 = (UINTN)(Address >> 32);
  Args.Arg2 = (UINT32)Address;
  Result    = TEEC_ERROR_COMMUNICATION;
  for (Count = 0; Count <= OPTEE_RPC_MAX_CALLS; Count++) {
    ArmCallSmc (&Args);
    if (OPTEE_SMC_RETURN_IS_RPC (Args.Arg0)) {
      if (Count == OPTEE_RPC_MAX_CALLS) {
        OpteeReject ("RPC loop bound", Count, TEEC_ERROR_COMMUNICATION);
        break;
      }

      if (!OpteeRpcHandle (&Args)) {
        break;
      }

      // a3-a7 are secure resume state, not scratch registers.
      Args.Arg0 = OPTEE_SMC_RETURN_FROM_RPC;
      continue;
    }

    if (Args.Arg0 == OPTEE_SMC_RETURN_OK) {
      Result = TEEC_SUCCESS;
    } else if (Args.Arg0 == OPTEE_SMC_RETURN_ETHREAD_LIMIT) {
      Result = TEEC_ERROR_BUSY;
    } else if (Args.Arg0 == OPTEE_SMC_RETURN_UNKNOWN_FUNCTION) {
      Result = TEEC_ERROR_NOT_IMPLEMENTED;
    }

    if (Result != TEEC_SUCCESS) {
      OpteeReject ("SMC return", Args.Arg0, Result);
    }

    break;
  }

  if (Result != TEEC_SUCCESS) {
    // A suspended secure thread cannot safely resume with recycled buffers.
    mBroken = TRUE;
    ReleaseRpcAllocations ();
  }

  mInCall = FALSE;
  return Result;
}

VOID
OpteeUuidFromOctets (
  IN  CONST UINT8  *Octets,
  OUT EFI_GUID     *Guid
  )
{
  Guid->Data1 = SwapBytes32 (ReadUnaligned32 ((CONST UINT32 *)Octets));
  Guid->Data2 = SwapBytes16 (ReadUnaligned16 ((CONST UINT16 *)(Octets + 4)));
  Guid->Data3 = SwapBytes16 (ReadUnaligned16 ((CONST UINT16 *)(Octets + 6)));
  CopyMem (Guid->Data4, Octets + 8, sizeof (Guid->Data4));
}

STATIC
VOID
UuidToOctets (
  IN  CONST TEEC_UUID  *Uuid,
  OUT UINT8            *Octets
  )
{
  WriteUnaligned32 ((UINT32 *)Octets, SwapBytes32 (Uuid->timeLow));
  WriteUnaligned16 ((UINT16 *)(Octets + 4), SwapBytes16 (Uuid->timeMid));
  WriteUnaligned16 ((UINT16 *)(Octets + 6), SwapBytes16 (Uuid->timeHiAndVersion));
  CopyMem (Octets + 8, Uuid->clockSeqAndNode, 8);
}

EFI_STATUS
OpteeClientApiInitialize (
  IN EFI_HANDLE  ImageHandle
  )
{
  EFI_STATUS  Status;

  if (mBroken) {
    return EFI_DEVICE_ERROR;
  }

  if (mInitialized) {
    return EFI_SUCCESS;
  }

  Status = OpteeShmInit ();
  if (!EFI_ERROR (Status)) {
    mInitialized = TRUE;
  }

  return Status;
}

BOOLEAN
OpteeClientApiFinalize (
  VOID
  )
{
  UINTN         Index;
  ARM_SMC_ARGS  Args;
  UINT64        Address;
  BOOLEAN       Drained;
  BOOLEAN       HaveRpc;

  if (mInCall) {
    return FALSE;
  }

  HaveRpc = FALSE;
  for (Index = 0; Index < mShmPages; Index++) {
    if ((mShm[Index].Pages != 0) && (mShm[Index].Owner == OPTEE_SHM_RPC_ARG)) {
      HaveRpc = TRUE;
      break;
    }
  }

  Drained = !HaveRpc;
  if (HaveRpc && !mBroken) {
    // Secure world can retain RPC arguments between calls. Drain its cache
    // before normal-world allocations become reusable by a later client.
    for (Index = 0; Index <= mShmPages; Index++) {
      ZeroMem (&Args, sizeof (Args));
      Args.Arg0 = OPTEE_SMC_DISABLE_SHM_CACHE;
      ArmCallSmc (&Args);
      if (Args.Arg0 == OPTEE_SMC_RETURN_ENOTAVAIL) {
        Drained = TRUE;
        break;
      }

      Address = ((UINT64)(UINT32)Args.Arg1 << 32) | (UINT32)Args.Arg2;
      if ((Args.Arg0 != OPTEE_SMC_RETURN_OK) ||
          !OpteeShmFreeOwned ((VOID *)(UINTN)Address, OPTEE_SHM_RPC_ARG))
      {
        OpteeReject ("SHM cache drain", (Args.Arg0 != OPTEE_SMC_RETURN_OK) ? Args.Arg0 : Address, TEEC_ERROR_COMMUNICATION);
        break;
      }
    }
  }

  if (!Drained) {
    mBroken = TRUE;
  }

  OpteeReeFsFinalize ();
  if (mShm != NULL) {
    for (Index = 0; Index < mShmPages; Index++) {
      if (mShm[Index].Pages != 0) {
        OpteeShmFreeOwned ((VOID *)(UINTN)(mShmBase + Index * SHM_PAGE_SIZE), mShm[Index].Owner);
      }
    }

    ZeroMem (mShm, mShmPages * sizeof (*mShm));
    FreePool (mShm);
  }

  mShm         = NULL;
  mShmBase     = 0;
  mShmPages    = 0;
  mInitialized = FALSE;
  mDelayMs     = 0;
  return Drained;
}

typedef struct {
  VOID               *Buffer[TEEC_CONFIG_PAYLOAD_REF_COUNT];
  UINTN              Capacity[TEEC_CONFIG_PAYLOAD_REF_COUNT];
  OPTEE_MSG_PARAM    Sent[TEEC_CONFIG_PAYLOAD_REF_COUNT];
} PARAM_BOUNCE;

STATIC
TEEC_Result
SetMsgParams (
  IN  TEEC_Operation   *Operation,
  OUT OPTEE_MSG_PARAM  *Params,
  OUT PARAM_BOUNCE     *Bounce
  )
{
  UINTN   Index;
  UINT32  Type;
  UINTN   Size;
  VOID    *Buffer;

  ZeroMem (Bounce, sizeof (*Bounce));
  if ((Operation != NULL) && ((Operation->paramTypes & 0xFFFF0000U) != 0)) {
    return TEEC_ERROR_BAD_PARAMETERS;
  }

  for (Index = 0; Index < TEEC_CONFIG_PAYLOAD_REF_COUNT; Index++) {
    Type = (Operation == NULL) ? TEEC_NONE : TEEC_PARAM_TYPE_GET (Operation->paramTypes, Index);
    switch (Type) {
      case TEEC_NONE:
        Params[Index].Attr = OPTEE_MSG_ATTR_TYPE_NONE;
        break;
      case TEEC_VALUE_INPUT:
      case TEEC_VALUE_OUTPUT:
      case TEEC_VALUE_INOUT:
        Params[Index].Attr = OPTEE_MSG_ATTR_TYPE_VALUE_INPUT + Type - TEEC_VALUE_INPUT;
        if (Type != TEEC_VALUE_OUTPUT) {
          Params[Index].U.Value.A = Operation->params[Index].value.a;
          Params[Index].U.Value.B = Operation->params[Index].value.b;
        }

        break;
      case TEEC_MEMREF_TEMP_INPUT:
      case TEEC_MEMREF_TEMP_OUTPUT:
      case TEEC_MEMREF_TEMP_INOUT:
        Size   = Operation->params[Index].tmpref.size;
        Buffer = Operation->params[Index].tmpref.buffer;
        if ((Buffer == NULL) && (Size != 0)) {
          return TEEC_ERROR_BAD_PARAMETERS;
        }

        Params[Index].Attr     = OPTEE_MSG_ATTR_TYPE_TMEM_INPUT + Type - TEEC_MEMREF_TEMP_INPUT;
        Bounce->Capacity[Index] = Size;
        if (Size != 0) {
          Bounce->Buffer[Index] = OpteeShmAlloc (Size, SHM_PAGE_SIZE);
          if (Bounce->Buffer[Index] == NULL) {
            return TEEC_ERROR_OUT_OF_MEMORY;
          }

          if (Type != TEEC_MEMREF_TEMP_OUTPUT) {
            CopyMem (Bounce->Buffer[Index], Buffer, Size);
          }
        }

        Params[Index].U.Tmem.BufPtr = (UINTN)Bounce->Buffer[Index];
        Params[Index].U.Tmem.Size   = Size;
        Params[Index].U.Tmem.ShmRef = (UINTN)Bounce->Buffer[Index];
        break;
      default:
        return TEEC_ERROR_NOT_SUPPORTED;
    }

    Bounce->Sent[Index] = Params[Index];
  }

  return TEEC_SUCCESS;
}

/** Validate every response before publishing any output. */
STATIC
TEEC_Result
GetMsgParams (
  IN     OPTEE_MSG_PARAM  *SharedParams,
  IN OUT TEEC_Operation   *Operation,
  IN     PARAM_BOUNCE     *Bounce,
  IN     TEEC_Result      Result
  )
{
  UINTN            Index;
  UINT64           Type;
  OPTEE_MSG_PARAM  Params[TEEC_CONFIG_PAYLOAD_REF_COUNT];
  TEEC_Result      SecureResult;
  UINT64           RequiredSize;

  SecureResult = Result;
  RequiredSize = 0;
  if ((Result != TEEC_SUCCESS) && (Result != TEEC_ERROR_SHORT_BUFFER)) {
    OpteeReject ("secure result", Result, Result);
    return Result;
  }
  CopyMem (Params, SharedParams, sizeof (Params));

  for (Index = 0; Index < TEEC_CONFIG_PAYLOAD_REF_COUNT; Index++) {
    Type = Bounce->Sent[Index].Attr;
    if (Params[Index].Attr != Type) {
      OpteeReject ("response attribute", Params[Index].Attr, TEEC_ERROR_COMMUNICATION);
      return TEEC_ERROR_COMMUNICATION;
    }

    if ((Type >= OPTEE_MSG_ATTR_TYPE_TMEM_INPUT) && (Type <= OPTEE_MSG_ATTR_TYPE_TMEM_INOUT)) {
      if ((Params[Index].U.Tmem.BufPtr != Bounce->Sent[Index].U.Tmem.BufPtr) ||
          (Params[Index].U.Tmem.ShmRef != Bounce->Sent[Index].U.Tmem.ShmRef) ||
          (Params[Index].U.Tmem.Size > MAX_UINTN))
      {
        OpteeReject ("response memref", (Params[Index].U.Tmem.BufPtr != Bounce->Sent[Index].U.Tmem.BufPtr) ? Params[Index].U.Tmem.BufPtr : ((Params[Index].U.Tmem.ShmRef != Bounce->Sent[Index].U.Tmem.ShmRef) ? Params[Index].U.Tmem.ShmRef : Params[Index].U.Tmem.Size), TEEC_ERROR_COMMUNICATION);
        return TEEC_ERROR_COMMUNICATION;
      }

      if ((Type != OPTEE_MSG_ATTR_TYPE_TMEM_INPUT) &&
          (Params[Index].U.Tmem.Size > Bounce->Capacity[Index]))
      {
        if (Result != TEEC_ERROR_SHORT_BUFFER) {
          RequiredSize = Params[Index].U.Tmem.Size;
        }
        Result = TEEC_ERROR_SHORT_BUFFER;
      }
    }
  }
  if (SecureResult == TEEC_ERROR_SHORT_BUFFER) {
    OpteeReject ("secure result", SecureResult, SecureResult);
  } else if (RequiredSize != 0) {
    OpteeReject ("response capacity", RequiredSize, TEEC_ERROR_SHORT_BUFFER);
  }

  if (Operation == NULL) {
    return Result;
  }

  for (Index = 0; Index < TEEC_CONFIG_PAYLOAD_REF_COUNT; Index++) {
    Type = Bounce->Sent[Index].Attr;
    if ((Type == OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT) || (Type == OPTEE_MSG_ATTR_TYPE_TMEM_INOUT)) {
      if ((Result == TEEC_SUCCESS) && (Params[Index].U.Tmem.Size != 0)) {
        CopyMem (Operation->params[Index].tmpref.buffer, Bounce->Buffer[Index], (UINTN)Params[Index].U.Tmem.Size);
      }

      Operation->params[Index].tmpref.size = (UINTN)Params[Index].U.Tmem.Size;
    } else if ((Result == TEEC_SUCCESS) &&
               ((Type == OPTEE_MSG_ATTR_TYPE_VALUE_OUTPUT) || (Type == OPTEE_MSG_ATTR_TYPE_VALUE_INOUT)))
    {
      Operation->params[Index].value.a = (UINT32)Params[Index].U.Value.A;
      Operation->params[Index].value.b = (UINT32)Params[Index].U.Value.B;
    }
  }

  return Result;
}

STATIC
VOID
FreeBounce (
  IN PARAM_BOUNCE  *Bounce
  )
{
  UINTN  Index;

  for (Index = 0; Index < TEEC_CONFIG_PAYLOAD_REF_COUNT; Index++) {
    OpteeShmFree (Bounce->Buffer[Index]);
  }
}

TEEC_Result
TEEC_InitializeContext (
  const char    *name,
  TEEC_Context  *context
  )
{
  if ((name != NULL) || (context == NULL)) {
    return TEEC_ERROR_BAD_PARAMETERS;
  }

  if (EFI_ERROR (OpteeClientApiInitialize (NULL))) {
    return TEEC_ERROR_COMMUNICATION;
  }

  context->fd = 1;
  return TEEC_SUCCESS;
}

void
TEEC_FinalizeContext (
  TEEC_Context  *context
  )
{
  if (context != NULL) {
    context->fd = -1;
  }
}

TEEC_Result
TEEC_OpenSession (
  TEEC_Context     *context,
  TEEC_Session     *session,
  const TEEC_UUID  *destination,
  uint32_t         connectionMethod,
  const void       *connectionData,
  TEEC_Operation   *operation,
  uint32_t         *returnOrigin
  )
{
  OPTEE_MSG_ARG  *Arg;
  PARAM_BOUNCE   Bounce;
  TEEC_Result    Result;
  UINT32         Origin;
  UINT32         OpenedSession;
  TEEC_Result    SecureResult;

  Origin = TEEC_ORIGIN_API;
  Result = TEEC_ERROR_BAD_PARAMETERS;
  if ((context == NULL) || (context->fd != 1) || (session == NULL) ||
      (destination == NULL) || (connectionMethod != TEEC_LOGIN_PUBLIC) ||
      (connectionData != NULL) || !mInitialized || mBroken)
  {
    goto Done;
  }

  Arg = OpteeShmAlloc (OPTEE_MSG_ARG_SIZE (TEEC_CONFIG_PAYLOAD_REF_COUNT + 2), SHM_PAGE_SIZE);
  if (Arg == NULL) {
    Result = TEEC_ERROR_OUT_OF_MEMORY;
    goto Done;
  }

  Arg->Cmd           = OPTEE_MSG_CMD_OPEN_SESSION;
  Arg->NumParams     = TEEC_CONFIG_PAYLOAD_REF_COUNT + 2;
  Arg->Params[0].Attr = OPTEE_MSG_ATTR_TYPE_VALUE_INPUT | OPTEE_MSG_ATTR_META;
  UuidToOctets (destination, (UINT8 *)&Arg->Params[0].U.Value);
  Arg->Params[1].Attr      = OPTEE_MSG_ATTR_TYPE_VALUE_INPUT | OPTEE_MSG_ATTR_META;
  Arg->Params[1].U.Value.C = TEEC_LOGIN_PUBLIC;
  Result = SetMsgParams (operation, &Arg->Params[2], &Bounce);
  if (Result == TEEC_SUCCESS) {
    Origin = TEEC_ORIGIN_COMMS;
    Result = OpteeCallWithArg (Arg);
    if (Result == TEEC_SUCCESS) {
      OpenedSession = Arg->Session;
      SecureResult  = Arg->Ret;
      if ((Arg->Cmd != OPTEE_MSG_CMD_OPEN_SESSION) ||
          (Arg->NumParams != TEEC_CONFIG_PAYLOAD_REF_COUNT + 2) ||
          (Arg->Params[0].Attr != (OPTEE_MSG_ATTR_TYPE_VALUE_INPUT | OPTEE_MSG_ATTR_META)) ||
          (Arg->Params[1].Attr != (OPTEE_MSG_ATTR_TYPE_VALUE_INPUT | OPTEE_MSG_ATTR_META)))
      {
        OpteeReject ("open response framing", (Arg->Cmd != OPTEE_MSG_CMD_OPEN_SESSION) ? Arg->Cmd : ((Arg->NumParams != TEEC_CONFIG_PAYLOAD_REF_COUNT + 2) ? Arg->NumParams : ((Arg->Params[0].Attr != (OPTEE_MSG_ATTR_TYPE_VALUE_INPUT | OPTEE_MSG_ATTR_META)) ? Arg->Params[0].Attr : Arg->Params[1].Attr)), TEEC_ERROR_COMMUNICATION);
        Result = TEEC_ERROR_COMMUNICATION;
      } else {
        Result = GetMsgParams (&Arg->Params[2], operation, &Bounce, SecureResult);
        Origin = Arg->RetOrigin;
        if (Result == TEEC_SUCCESS) {
          session->ctx        = context;
          session->session_id = OpenedSession;
        }
      }

      if ((SecureResult == TEEC_SUCCESS) && (Result != TEEC_SUCCESS)) {
        // A rejected reply must not orphan the session that OP-TEE opened.
        // Reuse the live argument allocation so cleanup cannot fail on OOM.
        ZeroMem (Arg, OPTEE_MSG_ARG_SIZE (TEEC_CONFIG_PAYLOAD_REF_COUNT + 2));
        Arg->Cmd     = OPTEE_MSG_CMD_CLOSE_SESSION;
        Arg->Session = OpenedSession;
        SecureResult = OpteeCallWithArg (Arg);
        if (SecureResult == TEEC_SUCCESS) {
          if ((Arg->Ret != TEEC_SUCCESS) || (Arg->Cmd != OPTEE_MSG_CMD_CLOSE_SESSION) ||
              (Arg->Session != OpenedSession) || (Arg->NumParams != 0))
          {
            OpteeReject ("rejected open cleanup", (Arg->Ret != TEEC_SUCCESS) ? Arg->Ret : ((Arg->Cmd != OPTEE_MSG_CMD_CLOSE_SESSION) ? Arg->Cmd : ((Arg->NumParams != 0) ? Arg->NumParams : Arg->Session)), TEEC_ERROR_COMMUNICATION);
            mBroken = TRUE;
            ReleaseRpcAllocations ();
          }
        } else {
          mBroken = TRUE;
          ReleaseRpcAllocations ();
        }
      }
    }
  }

  FreeBounce (&Bounce);
  OpteeShmFree (Arg);
Done:
  if (returnOrigin != NULL) {
    *returnOrigin = Origin;
  }

  return Result;
}

TEEC_Result
OpteeClientCloseSession (
  IN TEEC_Session  *Session
  )
{
  OPTEE_MSG_ARG  *Arg;
  TEEC_Result    Result;

  if ((Session == NULL) || (Session->ctx == NULL) || (Session->ctx->fd != 1) ||
      !mInitialized || mBroken)
  {
    return TEEC_ERROR_BAD_PARAMETERS;
  }

  Arg = OpteeShmAlloc (OPTEE_MSG_ARG_SIZE (0), SHM_PAGE_SIZE);
  if (Arg == NULL) {
    return TEEC_ERROR_OUT_OF_MEMORY;
  }

  Arg->Cmd     = OPTEE_MSG_CMD_CLOSE_SESSION;
  Arg->Session = Session->session_id;
  Result       = OpteeCallWithArg (Arg);
  if (Result == TEEC_SUCCESS) {
    if ((Arg->Cmd != OPTEE_MSG_CMD_CLOSE_SESSION) || (Arg->Session != Session->session_id) ||
        (Arg->NumParams != 0))
    {
      OpteeReject ("close response framing", (Arg->Cmd != OPTEE_MSG_CMD_CLOSE_SESSION) ? Arg->Cmd : ((Arg->NumParams != 0) ? Arg->NumParams : Arg->Session), TEEC_ERROR_COMMUNICATION);
      Result = TEEC_ERROR_COMMUNICATION;
    } else {
      Result = Arg->Ret;
      if (Result != TEEC_SUCCESS) {
        OpteeReject ("close secure result", Result, Result);
      }
    }
  }

  OpteeShmFree (Arg);
  if (Result == TEEC_SUCCESS) {
    ZeroMem (Session, sizeof (*Session));
  }

  return Result;
}

void
TEEC_CloseSession (
  TEEC_Session  *session
  )
{
  OpteeClientCloseSession (session);
}

TEEC_Result
TEEC_InvokeCommand (
  TEEC_Session    *session,
  uint32_t        commandID,
  TEEC_Operation  *operation,
  uint32_t        *returnOrigin
  )
{
  OPTEE_MSG_ARG  *Arg;
  PARAM_BOUNCE   Bounce;
  TEEC_Result    Result;
  UINT32         Origin;

  Origin = TEEC_ORIGIN_API;
  Result = TEEC_ERROR_BAD_PARAMETERS;
  if ((session == NULL) || (session->ctx == NULL) || (session->ctx->fd != 1) ||
      !mInitialized || mBroken)
  {
    goto Done;
  }

  Arg = OpteeShmAlloc (OPTEE_MSG_ARG_SIZE (TEEC_CONFIG_PAYLOAD_REF_COUNT), SHM_PAGE_SIZE);
  if (Arg == NULL) {
    Result = TEEC_ERROR_OUT_OF_MEMORY;
    goto Done;
  }

  Arg->Cmd       = OPTEE_MSG_CMD_INVOKE_COMMAND;
  Arg->Func      = commandID;
  Arg->Session   = session->session_id;
  Arg->NumParams = TEEC_CONFIG_PAYLOAD_REF_COUNT;
  Result = SetMsgParams (operation, Arg->Params, &Bounce);
  if (Result == TEEC_SUCCESS) {
    Origin = TEEC_ORIGIN_COMMS;
    Result = OpteeCallWithArg (Arg);
    if (Result == TEEC_SUCCESS) {
      if ((Arg->Cmd != OPTEE_MSG_CMD_INVOKE_COMMAND) || (Arg->Func != commandID) ||
          (Arg->Session != session->session_id) || (Arg->NumParams != TEEC_CONFIG_PAYLOAD_REF_COUNT))
      {
        OpteeReject ("invoke response framing", (Arg->Cmd != OPTEE_MSG_CMD_INVOKE_COMMAND) ? Arg->Cmd : ((Arg->NumParams != TEEC_CONFIG_PAYLOAD_REF_COUNT) ? Arg->NumParams : ((Arg->Func != commandID) ? Arg->Func : Arg->Session)), TEEC_ERROR_COMMUNICATION);
        Result = TEEC_ERROR_COMMUNICATION;
      } else {
        Result = GetMsgParams (Arg->Params, operation, &Bounce, Arg->Ret);
        Origin = Arg->RetOrigin;
      }
    }
  }

  FreeBounce (&Bounce);
  OpteeShmFree (Arg);
Done:
  if (returnOrigin != NULL) {
    *returnOrigin = Origin;
  }

  return Result;
}
