/** @file
  OP-TEE RPC handling: the subset of tee-supplicant and the Linux OP-TEE
  driver that a single-threaded firmware client needs.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include "OpteeClientInternal.h"

STATIC
BOOLEAN
IsValueParam (
  IN OPTEE_MSG_PARAM  *Param
  )
{
  UINT64  Type;

  Type = Param->Attr & OPTEE_MSG_ATTR_TYPE_MASK;
  return Type >= OPTEE_MSG_ATTR_TYPE_VALUE_INPUT && Type <= OPTEE_MSG_ATTR_TYPE_VALUE_INOUT;
}

/**
  [in]  value[0].a-b  TA UUID as octets
  [out] memref[1]     TA image; a NULL buffer queries the size
**/
STATIC
TEEC_Result
RpcLoadTa (
  IN OUT OPTEE_MSG_ARG  *Arg
  )
{
  EFI_GUID    Uuid;
  VOID        *Image;
  UINTN       ImageSize;
  VOID        *Buffer;
  UINTN       BufferSize;
  TEEC_Result Result;

  if ((Arg->NumParams != 2) || !IsValueParam (&Arg->Params[0]) ||
      !OpteeMsgParamToBuffer (&Arg->Params[1], &Buffer, &BufferSize))
  {
    return TEEC_ERROR_BAD_PARAMETERS;
  }

  OpteeUuidFromOctets ((CONST UINT8 *)&Arg->Params[0].U.Value, &Uuid);

  //
  // TAs ship as FREEFORM files named by their UUID in a firmware volume.
  //
  if (EFI_ERROR (GetSectionFromAnyFv (&Uuid, EFI_SECTION_RAW, 0, &Image, &ImageSize))) {
    DEBUG ((DEBUG_ERROR, "%a: TA %g not found\n", __func__, &Uuid));
    return TEEC_ERROR_ITEM_NOT_FOUND;
  }

  OpteeMsgParamSetSize (&Arg->Params[1], ImageSize);
  if (Buffer == NULL) {
    Result = TEEC_SUCCESS;
  } else if (ImageSize > BufferSize) {
    Result = TEEC_ERROR_SHORT_BUFFER;
  } else {
    CopyMem (Buffer, Image, ImageSize);
    DEBUG ((DEBUG_INFO, "%a: loaded TA %g (0x%lx bytes)\n", __func__, &Uuid, (UINT64)ImageSize));
    Result = TEEC_SUCCESS;
  }

  FreePool (Image);
  return Result;
}

STATIC
UINT64
DaysFromCivil (
  IN UINT64  Year,
  IN UINT64  Month,
  IN UINT64  Day
  )
{
  UINT64  Era;
  UINT64  YearOfEra;
  UINT64  DayOfYear;
  UINT64  DayOfEra;

  Year     -= (Month <= 2) ? 1 : 0;
  Era       = Year / 400;
  YearOfEra = Year - Era * 400;
  DayOfYear = (153 * (Month + ((Month > 2) ? -3 : 9)) + 2) / 5 + Day - 1;
  DayOfEra  = YearOfEra * 365 + YearOfEra / 4 - YearOfEra / 100 + DayOfYear;
  return Era * 146097 + DayOfEra - 719468;
}

/**
  [out] value[0].a  seconds since the Epoch
  [out] value[0].b  nanoseconds
**/
STATIC
TEEC_Result
RpcGetTime (
  IN OUT OPTEE_MSG_ARG  *Arg
  )
{
  EFI_TIME  Time;

  if ((Arg->NumParams != 1) || !IsValueParam (&Arg->Params[0])) {
    return TEEC_ERROR_BAD_PARAMETERS;
  }

  if (EFI_ERROR (gRT->GetTime (&Time, NULL))) {
    return TEEC_ERROR_GENERIC;
  }

  Arg->Params[0].U.Value.A = DaysFromCivil (Time.Year, Time.Month, Time.Day) * 86400 +
                             Time.Hour * 3600 + Time.Minute * 60 + Time.Second;
  if (Time.TimeZone != EFI_UNSPECIFIED_TIMEZONE) {
    Arg->Params[0].U.Value.A += (INT64)Time.TimeZone * 60;
  }

  Arg->Params[0].U.Value.B = Time.Nanosecond;
  return TEEC_SUCCESS;
}

/**
  [in] value[0].a  type, value[0].b size, value[0].c alignment
  [out] memref[0]  buffer
**/
STATIC
TEEC_Result
RpcShmAlloc (
  IN OUT OPTEE_MSG_ARG  *Arg
  )
{
  VOID  *Buffer;
  UINTN Size;

  if ((Arg->NumParams != 1) || !IsValueParam (&Arg->Params[0])) {
    return TEEC_ERROR_BAD_PARAMETERS;
  }

  Size   = (UINTN)Arg->Params[0].U.Value.B;
  Buffer = OpteeShmAlloc (Size, (UINTN)Arg->Params[0].U.Value.C);
  if (Buffer == NULL) {
    return TEEC_ERROR_OUT_OF_MEMORY;
  }

  ZeroMem (Buffer, Size);
  Arg->Params[0].Attr          = OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT;
  Arg->Params[0].U.Tmem.BufPtr = (UINT64)(UINTN)Buffer;
  Arg->Params[0].U.Tmem.Size   = Size;
  Arg->Params[0].U.Tmem.ShmRef = (UINT64)(UINTN)Buffer;
  return TEEC_SUCCESS;
}

/**
  [in] value[0].a  type, value[0].b cookie
**/
STATIC
TEEC_Result
RpcShmFree (
  IN OUT OPTEE_MSG_ARG  *Arg
  )
{
  if ((Arg->NumParams != 1) || !IsValueParam (&Arg->Params[0])) {
    return TEEC_ERROR_BAD_PARAMETERS;
  }

  OpteeShmFree ((VOID *)(UINTN)Arg->Params[0].U.Value.B);
  return TEEC_SUCCESS;
}

STATIC
VOID
RpcCmd (
  IN OUT OPTEE_MSG_ARG  *Arg
  )
{
  TEEC_Result  Result;

  switch (Arg->Cmd) {
    case OPTEE_RPC_CMD_LOAD_TA:
      Result = RpcLoadTa (Arg);
      break;

    case OPTEE_RPC_CMD_FS:
      Result = OpteeReeFsHandle (Arg);
      break;

    case OPTEE_RPC_CMD_GET_TIME:
      Result = RpcGetTime (Arg);
      break;

    case OPTEE_RPC_CMD_NOTIFICATION:
      //
      // Only one firmware thread ever runs in OP-TEE, so nobody can be
      // waiting for a notification that firmware would have to deliver.
      //
      Result = TEEC_SUCCESS;
      break;

    case OPTEE_RPC_CMD_SUSPEND:
      if ((Arg->NumParams == 1) && IsValueParam (&Arg->Params[0])) {
        MicroSecondDelay ((UINTN)Arg->Params[0].U.Value.A * 1000);
        Result = TEEC_SUCCESS;
      } else {
        Result = TEEC_ERROR_BAD_PARAMETERS;
      }

      break;

    case OPTEE_RPC_CMD_SHM_ALLOC:
      Result = RpcShmAlloc (Arg);
      break;

    case OPTEE_RPC_CMD_SHM_FREE:
      Result = RpcShmFree (Arg);
      break;

    default:
      DEBUG ((DEBUG_WARN, "%a: unsupported RPC command %u\n", __func__, Arg->Cmd));
      Result = TEEC_ERROR_NOT_SUPPORTED;
      break;
  }

  if (Result != TEEC_SUCCESS) {
    DEBUG ((DEBUG_INFO, "%a: RPC command %u -> 0x%x\n", __func__, Arg->Cmd, Result));
  }

  Arg->Ret = Result;
}

VOID
OpteeRpcHandle (
  IN OUT ARM_SMC_ARGS  *Args
  )
{
  UINT64         Address;
  VOID           *Buffer;
  OPTEE_MSG_ARG  *Arg;

  switch (OPTEE_SMC_RPC_FUNC (Args->Arg0)) {
    case OPTEE_SMC_RPC_FUNC_ALLOC:
      //
      // [in] a1 size; [out] a1/a2 physical address, a4/a5 cookie.
      //
      Buffer    = OpteeShmAlloc (Args->Arg1, SIZE_4KB);
      Address   = (UINT64)(UINTN)Buffer;
      Args->Arg1 = (UINTN)(Address >> 32);
      Args->Arg2 = (UINTN)(Address & MAX_UINT32);
      Args->Arg4 = Args->Arg1;
      Args->Arg5 = Args->Arg2;
      break;

    case OPTEE_SMC_RPC_FUNC_FREE:
      OpteeShmFree ((VOID *)(UINTN)(((UINT64)Args->Arg1 << 32) | (UINT32)Args->Arg2));
      break;

    case OPTEE_SMC_RPC_FUNC_FOREIGN_INTR:
      //
      // The interrupt has already been taken in normal world.
      //
      break;

    case OPTEE_SMC_RPC_FUNC_CMD:
      Address = ((UINT64)Args->Arg1 << 32) | (UINT32)Args->Arg2;
      if (!OpteeShmContains (Address, sizeof (OPTEE_MSG_ARG))) {
        DEBUG ((DEBUG_ERROR, "%a: RPC argument 0x%lx outside shared memory\n", __func__, Address));
        break;
      }

      Arg = (OPTEE_MSG_ARG *)(UINTN)Address;
      if (!OpteeShmContains (Address, OPTEE_MSG_ARG_SIZE (Arg->NumParams))) {
        Arg->Ret = TEEC_ERROR_BAD_PARAMETERS;
        break;
      }

      RpcCmd (Arg);
      break;

    default:
      DEBUG ((DEBUG_ERROR, "%a: unknown RPC function 0x%lx\n", __func__, Args->Arg0));
      break;
  }
}
