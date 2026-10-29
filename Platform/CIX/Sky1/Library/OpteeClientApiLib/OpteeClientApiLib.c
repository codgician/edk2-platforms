/** @file
  GlobalPlatform TEE Client API over the OP-TEE SMC interface.

  Firmware acts as the whole normal-world stack: it owns OP-TEE's reserved
  shared memory, issues standard calls and services every RPC, including the
  ones Linux forwards to tee-supplicant.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/ArmMmuLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/OpteeClientApiLib.h>

#include "OpteeClientInternal.h"

#define SHM_PAGE_SIZE  SIZE_4KB

STATIC BOOLEAN  mInitialized;
STATIC UINT64   mShmBase;
STATIC UINTN    mShmPages;
//
// Run length of each allocation, stored at its first page; zero for free
// pages and for pages inside a run.
//
STATIC UINT32  *mShmRun;
STATIC UINT8   *mShmUsed;

EFI_STATUS
OpteeShmInit (
  VOID
  )
{
  ARM_SMC_ARGS  Args;
  UINT64        Size;
  EFI_STATUS    Status;

  ZeroMem (&Args, sizeof (Args));
  Args.Arg0 = OPTEE_SMC_EXCHANGE_CAPABILITIES;
  ArmCallSmc (&Args);
  if (Args.Arg0 != OPTEE_SMC_RETURN_OK) {
    DEBUG ((DEBUG_ERROR, "%a: OP-TEE not present (0x%lx)\n", __func__, Args.Arg0));
    return EFI_UNSUPPORTED;
  }

  if ((Args.Arg1 & OPTEE_SMC_SEC_CAP_HAVE_RESERVED_SHM) == 0) {
    DEBUG ((DEBUG_ERROR, "%a: OP-TEE has no reserved shared memory\n", __func__));
    return EFI_UNSUPPORTED;
  }

  ZeroMem (&Args, sizeof (Args));
  Args.Arg0 = OPTEE_SMC_GET_SHM_CONFIG;
  ArmCallSmc (&Args);
  if (Args.Arg0 != OPTEE_SMC_RETURN_OK) {
    DEBUG ((DEBUG_ERROR, "%a: GET_SHM_CONFIG failed (0x%lx)\n", __func__, Args.Arg0));
    return EFI_UNSUPPORTED;
  }

  mShmBase = ALIGN_VALUE (Args.Arg1, SHM_PAGE_SIZE);
  Size     = Args.Arg2 - (mShmBase - Args.Arg1);
  if (Size < SHM_PAGE_SIZE) {
    return EFI_UNSUPPORTED;
  }

  mShmPages = (UINTN)(Size / SHM_PAGE_SIZE);
  DEBUG ((
    DEBUG_INFO,
    "%a: shared memory 0x%lx+0x%lx %a\n",
    __func__,
    mShmBase,
    (UINT64)mShmPages * SHM_PAGE_SIZE,
    (Args.Arg3 & OPTEE_SMC_SHM_CACHED) ? "cached" : "uncached"
    ));

  Status = ArmSetMemoryAttributes (
             mShmBase,
             (UINT64)mShmPages * SHM_PAGE_SIZE,
             (Args.Arg3 & OPTEE_SMC_SHM_CACHED) ? EFI_MEMORY_WB : EFI_MEMORY_UC
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: mapping shared memory failed: %r\n", __func__, Status));
    return Status;
  }

  mShmRun  = AllocateZeroPool (mShmPages * sizeof (*mShmRun));
  mShmUsed = AllocateZeroPool (mShmPages);
  if ((mShmRun == NULL) || (mShmUsed == NULL)) {
    return EFI_OUT_OF_RESOURCES;
  }

  return EFI_SUCCESS;
}

VOID *
OpteeShmAlloc (
  IN UINTN  Size,
  IN UINTN  Alignment
  )
{
  UINTN  Pages;
  UINTN  Step;
  UINTN  Start;
  UINTN  Index;

  if (!mInitialized) {
    return NULL;
  }

  Pages = MAX (EFI_SIZE_TO_PAGES (Size), 1);
  Step  = MAX (EFI_SIZE_TO_PAGES (Alignment), 1);

  for (Start = 0; Start + Pages <= mShmPages; Start += Step) {
    for (Index = 0; Index < Pages; Index++) {
      if (mShmUsed[Start + Index]) {
        break;
      }
    }

    if (Index == Pages) {
      SetMem (&mShmUsed[Start], Pages, 1);
      mShmRun[Start] = (UINT32)Pages;
      return (VOID *)(UINTN)(mShmBase + (UINT64)Start * SHM_PAGE_SIZE);
    }
  }

  DEBUG ((DEBUG_ERROR, "%a: out of shared memory (0x%lx bytes)\n", __func__, (UINT64)Size));
  return NULL;
}

VOID
OpteeShmFree (
  IN VOID  *Buffer
  )
{
  UINT64  Address;
  UINTN   Start;

  Address = (UINT64)(UINTN)Buffer;
  if ((Buffer == NULL) || !OpteeShmContains (Address, 1) || ((Address - mShmBase) % SHM_PAGE_SIZE != 0)) {
    return;
  }

  Start = (UINTN)((Address - mShmBase) / SHM_PAGE_SIZE);
  if (mShmRun[Start] == 0) {
    DEBUG ((DEBUG_ERROR, "%a: 0x%lx is not an allocation\n", __func__, Address));
    return;
  }

  SetMem (&mShmUsed[Start], mShmRun[Start], 0);
  mShmRun[Start] = 0;
}

BOOLEAN
OpteeShmContains (
  IN UINT64  Address,
  IN UINT64  Size
  )
{
  UINT64  End;

  End = mShmBase + (UINT64)mShmPages * SHM_PAGE_SIZE;
  return Address >= mShmBase && Address <= End && Size <= End - Address;
}

BOOLEAN
OpteeMsgParamToBuffer (
  IN  OPTEE_MSG_PARAM  *Param,
  OUT VOID             **Buffer,
  OUT UINTN            *Size
  )
{
  UINT64  Address;
  UINT64  Length;

  switch (Param->Attr & OPTEE_MSG_ATTR_TYPE_MASK) {
    case OPTEE_MSG_ATTR_TYPE_TMEM_INPUT:
    case OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT:
    case OPTEE_MSG_ATTR_TYPE_TMEM_INOUT:
      Address = Param->U.Tmem.BufPtr;
      Length  = Param->U.Tmem.Size;
      break;

    case OPTEE_MSG_ATTR_TYPE_RMEM_INPUT:
    case OPTEE_MSG_ATTR_TYPE_RMEM_OUTPUT:
    case OPTEE_MSG_ATTR_TYPE_RMEM_INOUT:
      //
      // Cookies handed to OP-TEE are physical addresses.
      //
      Address = Param->U.Rmem.ShmRef + Param->U.Rmem.Offs;
      Length  = Param->U.Rmem.Size;
      break;

    default:
      return FALSE;
  }

  if (Address == 0) {
    *Buffer = NULL;
    *Size   = (UINTN)Length;
    return TRUE;
  }

  if (!OpteeShmContains (Address, Length)) {
    DEBUG ((DEBUG_ERROR, "%a: 0x%lx+0x%lx outside shared memory\n", __func__, Address, Length));
    return FALSE;
  }

  *Buffer = (VOID *)(UINTN)Address;
  *Size   = (UINTN)Length;
  return TRUE;
}

VOID
OpteeMsgParamSetSize (
  IN OUT OPTEE_MSG_PARAM  *Param,
  IN     UINTN            Size
  )
{
  //
  // Tmem and Rmem keep the size in the same slot.
  //
  Param->U.Tmem.Size = Size;
}

TEEC_Result
OpteeCallWithArg (
  IN OPTEE_MSG_ARG  *Arg
  )
{
  ARM_SMC_ARGS  Args;
  UINT64        Address;

  Address = (UINT64)(UINTN)Arg;
  ZeroMem (&Args, sizeof (Args));
  Args.Arg0 = OPTEE_SMC_CALL_WITH_ARG;
  Args.Arg1 = (UINTN)(Address >> 32);
  Args.Arg2 = (UINTN)(Address & MAX_UINT32);

  for ( ; ;) {
    ArmCallSmc (&Args);

    if (OPTEE_SMC_RETURN_IS_RPC (Args.Arg0)) {
      //
      // Arg3 carries the suspended thread id and must be returned unchanged.
      //
      OpteeRpcHandle (&Args);
      Args.Arg0 = OPTEE_SMC_RETURN_FROM_RPC;
      continue;
    }

    if (Args.Arg0 == OPTEE_SMC_RETURN_ETHREAD_LIMIT) {
      //
      // Single-threaded firmware: every thread busy means OP-TEE is waiting
      // on something firmware cannot release.
      //
      DEBUG ((DEBUG_ERROR, "%a: OP-TEE thread limit reached\n", __func__));
      return TEEC_ERROR_BUSY;
    }

    if (Args.Arg0 == OPTEE_SMC_RETURN_OK) {
      return TEEC_SUCCESS;
    }

    DEBUG ((DEBUG_ERROR, "%a: SMC returned 0x%lx\n", __func__, Args.Arg0));
    return (Args.Arg0 == OPTEE_SMC_RETURN_UNKNOWN_FUNCTION) ? TEEC_ERROR_NOT_IMPLEMENTED : TEEC_ERROR_COMMUNICATION;
  }
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

  if (mInitialized) {
    return EFI_SUCCESS;
  }

  Status = OpteeShmInit ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mInitialized = TRUE;
  return EFI_SUCCESS;
}

BOOLEAN
OpteeClientApiFinalize (
  VOID
  )
{
  //
  // Called from ExitBootServices: drop all state without touching the
  // memory map. OP-TEE's shared memory is reserved and reused by the OS.
  //
  mInitialized = FALSE;
  return TRUE;
}

//
// Parameter marshalling. Temporary memory references are bounced through
// shared memory so callers may pass any buffer.
//
typedef struct {
  VOID     *Bounce[TEEC_CONFIG_PAYLOAD_REF_COUNT];
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

  ZeroMem (Bounce, sizeof (*Bounce));
  for (Index = 0; Index < TEEC_CONFIG_PAYLOAD_REF_COUNT; Index++) {
    Type = (Operation == NULL) ? TEEC_NONE : TEEC_PARAM_TYPE_GET (Operation->paramTypes, Index);
    switch (Type) {
      case TEEC_NONE:
        Params[Index].Attr = OPTEE_MSG_ATTR_TYPE_NONE;
        break;

      case TEEC_VALUE_INPUT:
      case TEEC_VALUE_OUTPUT:
      case TEEC_VALUE_INOUT:
        Params[Index].Attr      = OPTEE_MSG_ATTR_TYPE_VALUE_INPUT + (Type - TEEC_VALUE_INPUT);
        Params[Index].U.Value.A = Operation->params[Index].value.a;
        Params[Index].U.Value.B = Operation->params[Index].value.b;
        break;

      case TEEC_MEMREF_TEMP_INPUT:
      case TEEC_MEMREF_TEMP_OUTPUT:
      case TEEC_MEMREF_TEMP_INOUT:
        Params[Index].Attr = OPTEE_MSG_ATTR_TYPE_TMEM_INPUT + (Type - TEEC_MEMREF_TEMP_INPUT);
        if ((Operation->params[Index].tmpref.buffer != NULL) && (Operation->params[Index].tmpref.size != 0)) {
          Bounce->Bounce[Index] = OpteeShmAlloc (Operation->params[Index].tmpref.size, SHM_PAGE_SIZE);
          if (Bounce->Bounce[Index] == NULL) {
            return TEEC_ERROR_OUT_OF_MEMORY;
          }

          if (Type != TEEC_MEMREF_TEMP_OUTPUT) {
            CopyMem (Bounce->Bounce[Index], Operation->params[Index].tmpref.buffer, Operation->params[Index].tmpref.size);
          }
        }

        Params[Index].U.Tmem.BufPtr = (UINT64)(UINTN)Bounce->Bounce[Index];
        Params[Index].U.Tmem.Size   = Operation->params[Index].tmpref.size;
        Params[Index].U.Tmem.ShmRef = (UINT64)(UINTN)Bounce->Bounce[Index];
        break;

      default:
        return TEEC_ERROR_NOT_SUPPORTED;
    }
  }

  return TEEC_SUCCESS;
}

STATIC
VOID
GetMsgParams (
  IN     OPTEE_MSG_PARAM  *Params,
  IN OUT TEEC_Operation   *Operation,
  IN     PARAM_BOUNCE     *Bounce
  )
{
  UINTN   Index;
  UINT32  Type;
  UINTN   Copy;

  for (Index = 0; Index < TEEC_CONFIG_PAYLOAD_REF_COUNT; Index++) {
    Type = (Operation == NULL) ? TEEC_NONE : TEEC_PARAM_TYPE_GET (Operation->paramTypes, Index);
    switch (Type) {
      case TEEC_VALUE_OUTPUT:
      case TEEC_VALUE_INOUT:
        Operation->params[Index].value.a = (UINT32)Params[Index].U.Value.A;
        Operation->params[Index].value.b = (UINT32)Params[Index].U.Value.B;
        break;

      case TEEC_MEMREF_TEMP_OUTPUT:
      case TEEC_MEMREF_TEMP_INOUT:
        Copy = MIN ((UINTN)Params[Index].U.Tmem.Size, Operation->params[Index].tmpref.size);
        if ((Bounce->Bounce[Index] != NULL) && (Copy != 0)) {
          CopyMem (Operation->params[Index].tmpref.buffer, Bounce->Bounce[Index], Copy);
        }

        //
        // On TEEC_ERROR_SHORT_BUFFER this reports the required size.
        //
        Operation->params[Index].tmpref.size = (UINTN)Params[Index].U.Tmem.Size;
        break;

      default:
        break;
    }
  }
}

STATIC
VOID
FreeBounce (
  IN PARAM_BOUNCE  *Bounce
  )
{
  UINTN  Index;

  for (Index = 0; Index < TEEC_CONFIG_PAYLOAD_REF_COUNT; Index++) {
    OpteeShmFree (Bounce->Bounce[Index]);
  }
}

TEEC_Result
TEEC_InitializeContext (
  const char    *name,
  TEEC_Context  *context
  )
{
  if (context == NULL) {
    return TEEC_ERROR_BAD_PARAMETERS;
  }

  if (!mInitialized && EFI_ERROR (OpteeClientApiInitialize (NULL))) {
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
  uint32_t       Origin;

  Origin = TEEC_ORIGIN_API;
  if ((context == NULL) || (session == NULL) || (destination == NULL) ||
      (connectionMethod != TEEC_LOGIN_PUBLIC) || !mInitialized)
  {
    Result = TEEC_ERROR_BAD_PARAMETERS;
    goto Done;
  }

  Arg = OpteeShmAlloc (OPTEE_MSG_ARG_SIZE (TEEC_CONFIG_PAYLOAD_REF_COUNT + 2), SHM_PAGE_SIZE);
  if (Arg == NULL) {
    Result = TEEC_ERROR_OUT_OF_MEMORY;
    goto Done;
  }

  ZeroMem (Arg, OPTEE_MSG_ARG_SIZE (TEEC_CONFIG_PAYLOAD_REF_COUNT + 2));
  Arg->Cmd       = OPTEE_MSG_CMD_OPEN_SESSION;
  Arg->NumParams = TEEC_CONFIG_PAYLOAD_REF_COUNT + 2;

  Arg->Params[0].Attr = OPTEE_MSG_ATTR_TYPE_VALUE_INPUT | OPTEE_MSG_ATTR_META;
  UuidToOctets (destination, (UINT8 *)&Arg->Params[0].U.Value);
  Arg->Params[1].Attr      = OPTEE_MSG_ATTR_TYPE_VALUE_INPUT | OPTEE_MSG_ATTR_META;
  Arg->Params[1].U.Value.C = TEEC_LOGIN_PUBLIC;

  Result = SetMsgParams (operation, &Arg->Params[2], &Bounce);
  if (Result == TEEC_SUCCESS) {
    Origin = TEEC_ORIGIN_COMMS;
    Result = OpteeCallWithArg (Arg);
    if (Result == TEEC_SUCCESS) {
      session->session_id = Arg->Session;
      session->ctx        = context;
      Result              = Arg->Ret;
      Origin              = Arg->RetOrigin;
      GetMsgParams (&Arg->Params[2], operation, &Bounce);
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

void
TEEC_CloseSession (
  TEEC_Session  *session
  )
{
  OPTEE_MSG_ARG  *Arg;

  if ((session == NULL) || !mInitialized) {
    return;
  }

  Arg = OpteeShmAlloc (OPTEE_MSG_ARG_SIZE (0), SHM_PAGE_SIZE);
  if (Arg == NULL) {
    return;
  }

  ZeroMem (Arg, OPTEE_MSG_ARG_SIZE (0));
  Arg->Cmd     = OPTEE_MSG_CMD_CLOSE_SESSION;
  Arg->Session = session->session_id;
  if ((OpteeCallWithArg (Arg) != TEEC_SUCCESS) || (Arg->Ret != TEEC_SUCCESS)) {
    DEBUG ((DEBUG_ERROR, "%a: closing session 0x%x failed (0x%x)\n", __func__, session->session_id, Arg->Ret));
  }

  OpteeShmFree (Arg);
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
  uint32_t       Origin;

  Origin = TEEC_ORIGIN_API;
  if ((session == NULL) || !mInitialized) {
    Result = TEEC_ERROR_BAD_PARAMETERS;
    goto Done;
  }

  Arg = OpteeShmAlloc (OPTEE_MSG_ARG_SIZE (TEEC_CONFIG_PAYLOAD_REF_COUNT), SHM_PAGE_SIZE);
  if (Arg == NULL) {
    Result = TEEC_ERROR_OUT_OF_MEMORY;
    goto Done;
  }

  ZeroMem (Arg, OPTEE_MSG_ARG_SIZE (TEEC_CONFIG_PAYLOAD_REF_COUNT));
  Arg->Cmd       = OPTEE_MSG_CMD_INVOKE_COMMAND;
  Arg->Func      = commandID;
  Arg->Session   = session->session_id;
  Arg->NumParams = TEEC_CONFIG_PAYLOAD_REF_COUNT;

  Result = SetMsgParams (operation, Arg->Params, &Bounce);
  if (Result == TEEC_SUCCESS) {
    Origin = TEEC_ORIGIN_COMMS;
    Result = OpteeCallWithArg (Arg);
    if (Result == TEEC_SUCCESS) {
      Result = Arg->Ret;
      Origin = Arg->RetOrigin;
      GetMsgParams (Arg->Params, operation, &Bounce);
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
