/** @file
  OP-TEE RPC handling: the subset of tee-supplicant and the Linux OP-TEE
  driver that a single-threaded firmware client needs.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Pi/PiFirmwareFile.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include "OpteeClientInternal.h"

STATIC CONST EFI_GUID  mFtpmTaGuid = {
  0xbc50d971, 0xd4c9, 0x42c4, { 0x82, 0xcb, 0x34, 0x3f, 0xb7, 0xf3, 0x78, 0x96 }
};

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

  if ((Arg->NumParams != 2) ||
      (Arg->Params[0].Attr != OPTEE_MSG_ATTR_TYPE_VALUE_INPUT) ||
      (Arg->Params[1].Attr != OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT) ||
      !OpteeMsgParamToBuffer (&Arg->Params[1], &Buffer, &BufferSize))
  {
    return TEEC_ERROR_BAD_PARAMETERS;
  }

  OpteeUuidFromOctets ((CONST UINT8 *)&Arg->Params[0].U.Value, &Uuid);
  if (!CompareGuid (&Uuid, &mFtpmTaGuid)) {
    OpteeReject ("TA admission", Uuid.Data1, TEEC_ERROR_ITEM_NOT_FOUND);
    return TEEC_ERROR_ITEM_NOT_FOUND;
  }

  //
  // TAs ship as FREEFORM files named by their UUID in a firmware volume.
  //
  if (EFI_ERROR (GetSectionFromAnyFv (&Uuid, EFI_SECTION_RAW, 0, &Image, &ImageSize))) {
    DEBUG ((DEBUG_ERROR, "%a: TA %g not found\n", __func__, &Uuid));
    OpteeReject ("TA image unavailable", 0, TEEC_ERROR_ITEM_NOT_FOUND);
    return TEEC_ERROR_ITEM_NOT_FOUND;
  }

  OpteeMsgParamSetSize (&Arg->Params[1], ImageSize);
  if (Buffer == NULL) {
    Result = TEEC_SUCCESS;
  } else if (ImageSize > BufferSize) {
    OpteeReject ("TA capacity", ImageSize, TEEC_ERROR_SHORT_BUFFER);
    Result = TEEC_ERROR_SHORT_BUFFER;
  } else {
    CopyMem (Buffer, Image, ImageSize);
    DEBUG ((DEBUG_INFO, "%a: loaded TA %g (0x%lx bytes)\n", __func__, &Uuid, (UINT64)ImageSize));
    Result = TEEC_SUCCESS;
  }

  ZeroMem (Image, ImageSize);
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
  UINTN     MonthDays;
  STATIC CONST UINT8  DaysPerMonth[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

  if ((Arg->NumParams != 1) ||
      (Arg->Params[0].Attr != OPTEE_MSG_ATTR_TYPE_VALUE_OUTPUT)) {
    return TEEC_ERROR_BAD_PARAMETERS;
  }

  if (EFI_ERROR (gRT->GetTime (&Time, NULL))) {
    return TEEC_ERROR_GENERIC;
  }
  // Reject invalid civil fields before doing unsigned epoch arithmetic.
  if ((Time.Year < 1970) || (Time.Year > 9999) ||
      (Time.Month < 1) || (Time.Month > 12) ||
      (Time.Day < 1) || (Time.Day > 31) ||
      (Time.Hour > 23) || (Time.Minute > 59) || (Time.Second > 59) ||
      (Time.Nanosecond > 999999999) ||
      ((Time.TimeZone != EFI_UNSPECIFIED_TIMEZONE) &&
       ((Time.TimeZone < -1440) || (Time.TimeZone > 1440))))
  {
    return TEEC_ERROR_GENERIC;
  }

  MonthDays = DaysPerMonth[Time.Month - 1];
  if ((Time.Month == 2) && ((Time.Year % 4) == 0) &&
      (((Time.Year % 100) != 0) || ((Time.Year % 400) == 0)))
  {
    MonthDays++;
  }

  if (Time.Day > MonthDays) {
    return TEEC_ERROR_GENERIC;
  }

  Arg->Params[0].U.Value.A = DaysFromCivil (Time.Year, Time.Month, Time.Day) * 86400 +
                             Time.Hour * 3600 + Time.Minute * 60 + Time.Second;
  if (Time.TimeZone != EFI_UNSPECIFIED_TIMEZONE) {
    INT64  Seconds;

    Seconds = (INT64)Arg->Params[0].U.Value.A + (INT64)Time.TimeZone * 60;
    if (Seconds < 0) {
      return TEEC_ERROR_GENERIC;
    }

    Arg->Params[0].U.Value.A = (UINT64)Seconds;
  }

  Arg->Params[0].U.Value.B = Time.Nanosecond;
  return TEEC_SUCCESS;
}

STATIC
UINT32
RpcPayloadOwner (
  IN UINT64  Type
  )
{
  switch (Type) {
    case 0:
      return OPTEE_SHM_RPC_APPL;
    case 1:
      return OPTEE_SHM_RPC_KERNEL;
    case 2:
      return OPTEE_SHM_RPC_GLOBAL;
    default:
      return 0;
  }
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
  VOID    *Buffer;
  UINTN   Size;
  UINT32  Owner;

  if ((Arg->NumParams != 1) ||
      (Arg->Params[0].Attr != OPTEE_MSG_ATTR_TYPE_VALUE_INPUT)) {
    return TEEC_ERROR_BAD_PARAMETERS;
  }

  Owner = RpcPayloadOwner (Arg->Params[0].U.Value.A);
  if ((Owner == 0) || (Arg->Params[0].U.Value.B > MAX_UINTN) ||
      (Arg->Params[0].U.Value.C > MAX_UINTN) ||
      (Arg->Params[0].U.Value.C == 0) ||
      ((Arg->Params[0].U.Value.C & (Arg->Params[0].U.Value.C - 1)) != 0))
  {
    return TEEC_ERROR_BAD_PARAMETERS;
  }

  Size   = (UINTN)Arg->Params[0].U.Value.B;
  Buffer = OpteeShmAllocOwned (Size, (UINTN)Arg->Params[0].U.Value.C, Owner);
  if (Buffer == NULL) {
    return TEEC_ERROR_OUT_OF_MEMORY;
  }

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
  UINT32  Owner;

  if ((Arg->NumParams != 1) ||
      (Arg->Params[0].Attr != OPTEE_MSG_ATTR_TYPE_VALUE_INPUT)) {
    return TEEC_ERROR_BAD_PARAMETERS;
  }

  Owner = RpcPayloadOwner (Arg->Params[0].U.Value.A);
  if ((Owner == 0) || (Arg->Params[0].U.Value.B > MAX_UINTN) ||
      !OpteeShmFreeOwned ((VOID *)(UINTN)Arg->Params[0].U.Value.B, Owner))
  {
    return TEEC_ERROR_BAD_PARAMETERS;
  }
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
      if ((Arg->NumParams != 1) ||
          (Arg->Params[0].Attr != OPTEE_MSG_ATTR_TYPE_VALUE_INPUT))
      {
        Result = TEEC_ERROR_BAD_PARAMETERS;
      } else if (Arg->Params[0].U.Value.A == 1) {
        // SEND has no waiter to wake in this single-threaded client.
        Result = TEEC_SUCCESS;
      } else if (Arg->Params[0].U.Value.A == 0) {
        Result = TEEC_ERROR_NOT_SUPPORTED;
      } else {
        Result = TEEC_ERROR_BAD_PARAMETERS;
      }
      break;

    case OPTEE_RPC_CMD_SUSPEND:
      if ((Arg->NumParams == 1) &&
          (Arg->Params[0].Attr == OPTEE_MSG_ATTR_TYPE_VALUE_INPUT) &&
          OpteeRpcDelay (Arg->Params[0].U.Value.A))
      {
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
    // FS and TA admission/capacity report their own more specific check.
    if ((Arg->Cmd != OPTEE_RPC_CMD_FS) &&
        !((Arg->Cmd == OPTEE_RPC_CMD_LOAD_TA) &&
          ((Result == TEEC_ERROR_ITEM_NOT_FOUND) || (Result == TEEC_ERROR_SHORT_BUFFER))))
    {
      if (Arg->Cmd == OPTEE_RPC_CMD_SUSPEND) {
        OpteeReject ("RPC suspend", (Arg->NumParams != 1) ? Arg->NumParams : ((Arg->Params[0].Attr != OPTEE_MSG_ATTR_TYPE_VALUE_INPUT) ? Arg->Params[0].Attr : Arg->Params[0].U.Value.A), Result);
      } else if (Arg->Cmd == OPTEE_RPC_CMD_SHM_ALLOC) {
        OpteeReject ("RPC SHM allocation", (Arg->NumParams != 1) ? Arg->NumParams : ((Arg->Params[0].Attr != OPTEE_MSG_ATTR_TYPE_VALUE_INPUT) ? Arg->Params[0].Attr : ((RpcPayloadOwner (Arg->Params[0].U.Value.A) == 0) ? Arg->Params[0].U.Value.A : ((Arg->Params[0].U.Value.B > MAX_UINTN) ? Arg->Params[0].U.Value.B : ((Result == TEEC_ERROR_BAD_PARAMETERS) ? Arg->Params[0].U.Value.C : Arg->Params[0].U.Value.B)))), Result);
      } else if (Arg->Cmd == OPTEE_RPC_CMD_SHM_FREE) {
        OpteeReject ("RPC SHM ownership", (Arg->NumParams != 1) ? Arg->NumParams : ((Arg->Params[0].Attr != OPTEE_MSG_ATTR_TYPE_VALUE_INPUT) ? Arg->Params[0].Attr : ((RpcPayloadOwner (Arg->Params[0].U.Value.A) == 0) ? Arg->Params[0].U.Value.A : Arg->Params[0].U.Value.B)), Result);
      } else if ((Arg->Cmd == OPTEE_RPC_CMD_LOAD_TA) && (Arg->NumParams == 2)) {
        OpteeReject ("TA framing", (Arg->Params[0].Attr != OPTEE_MSG_ATTR_TYPE_VALUE_INPUT) ? Arg->Params[0].Attr : ((Arg->Params[1].Attr != OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT) ? Arg->Params[1].Attr : OpteeMsgParamRejectValue (&Arg->Params[1])), Result);
      } else if ((Arg->Cmd == OPTEE_RPC_CMD_NOTIFICATION) && (Arg->NumParams == 1)) {
        OpteeReject ("RPC notification", (Arg->Params[0].Attr != OPTEE_MSG_ATTR_TYPE_VALUE_INPUT) ? Arg->Params[0].Attr : Arg->Params[0].U.Value.A, Result);
      } else if ((Arg->Cmd == OPTEE_RPC_CMD_GET_TIME) && (Result == TEEC_ERROR_BAD_PARAMETERS) && (Arg->NumParams == 1)) {
        OpteeReject ("RPC time framing", Arg->Params[0].Attr, Result);
      } else if (Result == TEEC_ERROR_BAD_PARAMETERS) {
        OpteeReject ("RPC framing", Arg->NumParams, Result);
      } else {
        OpteeReject ("RPC command", Arg->Cmd, Result);
      }
    }
    DEBUG ((DEBUG_INFO, "%a: RPC command %u -> 0x%x\n", __func__, Arg->Cmd, Result));
  }

  Arg->Ret = Result;
}

/**
  Handle one RPC without retaining mutable shared metadata across services.
  FALSE means that no safe RPC response can be returned to secure world.
**/
BOOLEAN
OpteeRpcHandle (
  IN OUT ARM_SMC_ARGS  *Args
  )
{
  UINT64         Address;
  VOID           *Buffer;
  OPTEE_MSG_ARG  *Arg;
  UINTN          Length;
  UINT32         NumParams;
  UINT64         Storage[OPTEE_MSG_ARG_SIZE (OPTEE_RPC_MAX_PARAMS) / sizeof (UINT64)];
  OPTEE_MSG_ARG  *Snapshot;

  switch (OPTEE_SMC_RPC_FUNC (Args->Arg0)) {
    case OPTEE_SMC_RPC_FUNC_ALLOC:
      // Only a1/a2 and a4/a5 are allocation outputs; preserve resume state.
      Buffer    = OpteeShmAllocOwned (Args->Arg1, SIZE_4KB, OPTEE_SHM_RPC_ARG);
      if (Buffer == NULL) {
        OpteeReject ("RPC argument allocation", Args->Arg1, TEEC_ERROR_OUT_OF_MEMORY);
      }
      Address   = (UINT64)(UINTN)Buffer;
      Args->Arg1 = (UINTN)(Address >> 32);
      Args->Arg2 = (UINTN)(Address & MAX_UINT32);
      Args->Arg4 = Args->Arg1;
      Args->Arg5 = Args->Arg2;
      return TRUE;

    case OPTEE_SMC_RPC_FUNC_FREE:
      Address = ((UINT64)(UINT32)Args->Arg1 << 32) | (UINT32)Args->Arg2;
      if (!OpteeShmFreeOwned ((VOID *)(UINTN)Address, OPTEE_SHM_RPC_ARG)) {
        OpteeReject ("RPC argument ownership", Address, TEEC_ERROR_BAD_PARAMETERS);
        return FALSE;
      }

      return TRUE;

    case OPTEE_SMC_RPC_FUNC_FOREIGN_INTR:
      return TRUE;

    case OPTEE_SMC_RPC_FUNC_CMD:
      Address = ((UINT64)(UINT32)Args->Arg1 << 32) | (UINT32)Args->Arg2;
      if (!OpteeShmIsOwned (Address, sizeof (OPTEE_MSG_ARG), OPTEE_SHM_RPC_ARG)) {
        OpteeReject ("RPC argument ownership", Address, TEEC_ERROR_BAD_PARAMETERS);
        return FALSE;
      }

      Arg      = (OPTEE_MSG_ARG *)(UINTN)Address;
      Snapshot = (OPTEE_MSG_ARG *)Storage;
      CopyMem (Snapshot, Arg, sizeof (*Snapshot));
      NumParams = Snapshot->NumParams;
      if (NumParams > OPTEE_RPC_MAX_PARAMS) {
        OpteeReject ("RPC parameter count", NumParams, TEEC_ERROR_BAD_PARAMETERS);
        return FALSE;
      }

      Length = OPTEE_MSG_ARG_SIZE (NumParams);
      if (!OpteeShmIsOwned (Address, Length, OPTEE_SHM_RPC_ARG)) {
        OpteeReject ("RPC argument size", Length, TEEC_ERROR_BAD_PARAMETERS);
        return FALSE;
      }

      CopyMem (Snapshot->Params, Arg->Params, NumParams * sizeof (OPTEE_MSG_PARAM));
      RpcCmd (Snapshot);
      // Never size copyback from a header that secure world may have changed.
      CopyMem (Arg, Snapshot, Length);
      return TRUE;

    default:
      OpteeReject ("RPC function", OPTEE_SMC_RPC_FUNC (Args->Arg0), TEEC_ERROR_NOT_SUPPORTED);
      return FALSE;
  }
}
