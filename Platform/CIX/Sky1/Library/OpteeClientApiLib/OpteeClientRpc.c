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
#include <Library/OpteeTrustedAppGuids.h>

#include "OpteeClientInternal.h"

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

  if ((Arg->NumParams != 2) || (Arg->Params[0].Attr != OPTEE_MSG_ATTR_TYPE_VALUE_INPUT)) {
    OpteeReject ("TA framing", (Arg->NumParams != 2) ? Arg->NumParams : Arg->Params[0].Attr, TEEC_ERROR_BAD_PARAMETERS);
    return TEEC_ERROR_BAD_PARAMETERS;
  }

  if (!OpteeMsgParamToBuffer (&Arg->Params[1], OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT, "TA framing", &Buffer, &BufferSize)) {
    return TEEC_ERROR_BAD_PARAMETERS;
  }

  OpteeUuidFromOctets ((CONST UINT8 *)&Arg->Params[0].U.Value, &Uuid);
  if (!CompareGuid (&Uuid, &gOpteeFtpmTaGuid)) {
    OpteeReject ("TA admission", Uuid.Data1, TEEC_ERROR_ITEM_NOT_FOUND);
    return TEEC_ERROR_ITEM_NOT_FOUND;
  }

  //
  // TAs ship as FREEFORM files named by their UUID in a firmware volume.
  //
  if (EFI_ERROR (GetSectionFromAnyFv (&Uuid, EFI_SECTION_RAW, 0, &Image, &ImageSize))) {
    OpteeReject ("TA image unavailable", 0, TEEC_ERROR_ITEM_NOT_FOUND);
    return TEEC_ERROR_ITEM_NOT_FOUND;
  }

  Arg->Params[1].U.Tmem.Size = ImageSize;
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

/**
  SHM_ALLOC and SHM_FREE of application payloads; kernel and global payloads
  only serve requests this platform never makes.
  [in]  value[0].a  type, value[0].b size or cookie, value[0].c alignment
  [out] memref[0]   SHM_ALLOC buffer
**/
STATIC
TEEC_Result
RpcShm (
  IN OUT OPTEE_MSG_ARG  *Arg
  )
{
  VOID    *Buffer;
  UINT64  Value;

  if ((Arg->NumParams != 1) || (Arg->Params[0].Attr != OPTEE_MSG_ATTR_TYPE_VALUE_INPUT)) {
    OpteeReject ("RPC SHM framing", (Arg->NumParams != 1) ? Arg->NumParams : Arg->Params[0].Attr, TEEC_ERROR_BAD_PARAMETERS);
    return TEEC_ERROR_BAD_PARAMETERS;
  }

  if (Arg->Params[0].U.Value.A != OPTEE_RPC_SHM_TYPE_APPL) {
    OpteeReject ("RPC SHM type", Arg->Params[0].U.Value.A, TEEC_ERROR_BAD_PARAMETERS);
    return TEEC_ERROR_BAD_PARAMETERS;
  }

  Value = Arg->Params[0].U.Value.B;
  if (Arg->Cmd == OPTEE_RPC_CMD_SHM_FREE) {
    if (!OpteeShmFreeOwned ((VOID *)(UINTN)Value, OPTEE_SHM_RPC_APPL)) {
      OpteeReject ("RPC SHM ownership", Value, TEEC_ERROR_BAD_PARAMETERS);
      return TEEC_ERROR_BAD_PARAMETERS;
    }

    return TEEC_SUCCESS;
  }

  // The allocator itself rejects bad sizes and alignments.
  Buffer = OpteeShmAllocOwned ((UINTN)Value, (UINTN)Arg->Params[0].U.Value.C, OPTEE_SHM_RPC_APPL);
  if (Buffer == NULL) {
    OpteeReject ("RPC SHM allocation", Value, TEEC_ERROR_OUT_OF_MEMORY);
    return TEEC_ERROR_OUT_OF_MEMORY;
  }

  Arg->Params[0].Attr          = OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT;
  Arg->Params[0].U.Tmem.BufPtr = (UINT64)(UINTN)Buffer;
  Arg->Params[0].U.Tmem.Size   = Value;
  Arg->Params[0].U.Tmem.ShmRef = (UINT64)(UINTN)Buffer;
  return TEEC_SUCCESS;
}

STATIC
VOID
RpcCmd (
  IN OUT OPTEE_MSG_ARG  *Arg
  )
{
  switch (Arg->Cmd) {
    case OPTEE_RPC_CMD_LOAD_TA:
      Arg->Ret = RpcLoadTa (Arg);
      break;

    case OPTEE_RPC_CMD_FS:
      Arg->Ret = OpteeReeFsHandle (Arg);
      break;

    case OPTEE_RPC_CMD_SHM_ALLOC:
    case OPTEE_RPC_CMD_SHM_FREE:
      Arg->Ret = RpcShm (Arg);
      break;

    default:
      OpteeReject ("RPC command", Arg->Cmd, TEEC_ERROR_NOT_SUPPORTED);
      Arg->Ret = TEEC_ERROR_NOT_SUPPORTED;
      break;
  }
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
