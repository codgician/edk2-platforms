/** @file
  OP-TEE 3.x normal-world client internals: SMC/message ABI, shared-memory
  pool, RPC handlers and the volatile REE FS used while firmware acts as
  tee-supplicant.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef OPTEE_CLIENT_INTERNAL_H_
#define OPTEE_CLIENT_INTERNAL_H_

#include <Uefi.h>
#include <Library/ArmSmcLib.h>
#include <Library/tee_client_api.h>
#include "OpteeClientDiagnostics.h"

//
// SMC function IDs (optee_smc.h).
//
#define OPTEE_SMC_STD_CALL(Func)   (0x32000000U | (Func))
#define OPTEE_SMC_FAST_CALL(Func)  (0xB2000000U | (Func))

#define OPTEE_SMC_RETURN_FROM_RPC          OPTEE_SMC_STD_CALL (3)
#define OPTEE_SMC_CALL_WITH_ARG            OPTEE_SMC_STD_CALL (4)
#define OPTEE_SMC_GET_SHM_CONFIG           OPTEE_SMC_FAST_CALL (7)
#define OPTEE_SMC_EXCHANGE_CAPABILITIES    OPTEE_SMC_FAST_CALL (9)

#define OPTEE_SMC_RETURN_OK               0x0
#define OPTEE_SMC_RETURN_ETHREAD_LIMIT    0x1
#define OPTEE_SMC_RETURN_UNKNOWN_FUNCTION 0xFFFFFFFF
#define OPTEE_SMC_RETURN_RPC_PREFIX_MASK  0xFFFF0000
#define OPTEE_SMC_RETURN_RPC_PREFIX       0xFFFF0000
#define OPTEE_SMC_RETURN_IS_RPC(Ret) \
  ((((Ret) & OPTEE_SMC_RETURN_RPC_PREFIX_MASK) == OPTEE_SMC_RETURN_RPC_PREFIX) && \
   ((Ret) != OPTEE_SMC_RETURN_UNKNOWN_FUNCTION))
#define OPTEE_SMC_RPC_FUNC(Ret)  ((Ret) & 0xFFFF)

#define OPTEE_SMC_RPC_FUNC_ALLOC         0
#define OPTEE_SMC_RPC_FUNC_FREE          2
#define OPTEE_SMC_RPC_FUNC_FOREIGN_INTR  4
#define OPTEE_SMC_RPC_FUNC_CMD           5

#define OPTEE_SMC_SEC_CAP_HAVE_RESERVED_SHM  BIT0
#define OPTEE_SMC_SHM_CACHED                 1

//
// Message ABI (optee_msg.h).
//
#define OPTEE_MSG_ATTR_TYPE_NONE          0x0
#define OPTEE_MSG_ATTR_TYPE_VALUE_INPUT   0x1
#define OPTEE_MSG_ATTR_TYPE_VALUE_OUTPUT  0x2
#define OPTEE_MSG_ATTR_TYPE_VALUE_INOUT   0x3
#define OPTEE_MSG_ATTR_TYPE_TMEM_INPUT    0x9
#define OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT   0xa
#define OPTEE_MSG_ATTR_TYPE_TMEM_INOUT    0xb
#define OPTEE_MSG_ATTR_META               BIT8

#define OPTEE_MSG_CMD_OPEN_SESSION    0
#define OPTEE_MSG_CMD_INVOKE_COMMAND  1
#define OPTEE_MSG_CMD_CLOSE_SESSION   2

#define OPTEE_RPC_CMD_LOAD_TA       0
#define OPTEE_RPC_CMD_FS            2
#define OPTEE_RPC_CMD_SHM_ALLOC     6
#define OPTEE_RPC_CMD_SHM_FREE      7
#define OPTEE_RPC_SHM_TYPE_APPL     0

#define OPTEE_RPC_FS_OPEN      0
#define OPTEE_RPC_FS_CREATE    1
#define OPTEE_RPC_FS_CLOSE     2
#define OPTEE_RPC_FS_READ      3
#define OPTEE_RPC_FS_WRITE     4
#define OPTEE_RPC_FS_TRUNCATE  5
#define OPTEE_RPC_FS_REMOVE    6

#pragma pack(1)
typedef struct {
  UINT64    Attr;
  union {
    struct {
      UINT64    BufPtr;
      UINT64    Size;
      UINT64    ShmRef;
    } Tmem;
    struct {
      UINT64    A;
      UINT64    B;
      UINT64    C;
    } Value;
  } U;
} OPTEE_MSG_PARAM;

typedef struct {
  UINT32             Cmd;
  UINT32             Func;
  UINT32             Session;
  UINT32             CancelId;
  UINT32             Pad;
  UINT32             Ret;
  UINT32             RetOrigin;
  UINT32             NumParams;
  OPTEE_MSG_PARAM    Params[];
} OPTEE_MSG_ARG;
#pragma pack()

#define OPTEE_MSG_ARG_SIZE(NumParams) \
  (sizeof (OPTEE_MSG_ARG) + (NumParams) * sizeof (OPTEE_MSG_PARAM))

#define OPTEE_RPC_MAX_PARAMS  4
#define OPTEE_RPC_MAX_CALLS   1024
#define OPTEE_SHM_CLIENT      1
#define OPTEE_SHM_RPC_ARG     2
#define OPTEE_SHM_RPC_APPL    3

/**
  Allocate zeroed, page-backed static SHM with private ownership metadata.

  @param[in] Size       Requested accessible bytes; zero still reserves a page.
  @param[in] Alignment  Nonzero power of two, no larger than the pool.
  @param[in] Owner      Client, RPC argument, or RPC payload ownership class.
  @return Aligned allocation base, or NULL without changing live allocations.
**/
VOID *
OpteeShmAllocOwned (
  IN UINTN   Size,
  IN UINTN   Alignment,
  IN UINT32  Owner
  );

/**
  Scrub the entire page run and retire an exact, live owning allocation.

  @param[in] Buffer  Allocation base; interior/foreign/double frees are invalid.
  @param[in] Owner   Required ownership class.
  @retval TRUE   The owning allocation was scrubbed and released.
  @retval FALSE  Nothing was changed.
**/
BOOLEAN
OpteeShmFreeOwned (
  IN VOID    *Buffer,
  IN UINT32  Owner
  );

/**
  Check an exact allocation base, ownership class and requested byte extent.

  @param[in] Address  Exact base, never a pointer into another allocation.
  @param[in] Size     Required accessible bytes, excluding page padding.
  @param[in] Owner    Required ownership class.
  @retval TRUE  One live allocation satisfies the complete request.
**/
BOOLEAN
OpteeShmIsOwned (
  IN UINT64  Address,
  IN UINT64  Size,
  IN UINT32  Owner
  );

//
// Shared memory pool carved out of OP-TEE's reserved non-secure region.
// Physical and virtual addresses are identical in UEFI, so the physical
// address doubles as the shared-memory cookie.
//
EFI_STATUS
OpteeShmInit (
  VOID
  );

/**
  Resolve a static contiguous RPC payload of type Attr; registered/dynamic
  memory is absent. A non-NULL memref must carry the base cookie of its own
  live RPC payload. NULL requires zero size and zero cookie (LOAD_TA size
  query). A rejection emits one Check line with metadata, never the bytes.
**/
BOOLEAN
OpteeMsgParamToBuffer (
  IN  OPTEE_MSG_PARAM  *Param,
  IN  UINT64           Attr,
  IN  CONST CHAR8      *Check,
  OUT VOID             **Buffer,
  OUT UINTN            *Size
  );

/**
  Run one standard call, servicing at most OPTEE_RPC_MAX_CALLS RPCs.
  Transport/protocol failure scrubs RPC allocations and permanently poisons
  this client instance; it must never resume a suspended thread with reused SHM.
**/
TEEC_Result
OpteeCallWithArg (
  IN OPTEE_MSG_ARG  *Arg
  );

/** Return FALSE when no safe wire response exists; the caller must abort. */
BOOLEAN
OpteeRpcHandle (
  IN OUT ARM_SMC_ARGS  *Args
  );

//
// REE FS backend (OPTEE_RPC_CMD_FS).
//
TEEC_Result
OpteeReeFsHandle (
  IN OUT OPTEE_MSG_ARG  *Arg
  );

VOID
OpteeUuidFromOctets (
  IN  CONST UINT8  *Octets,
  OUT EFI_GUID     *Guid
  );

#endif // OPTEE_CLIENT_INTERNAL_H_
