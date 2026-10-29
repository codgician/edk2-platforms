/** @file
  OP-TEE 3.x normal-world client internals: SMC/message ABI, shared-memory
  pool, RPC handlers and the variable-backed REE FS used while firmware acts
  as tee-supplicant.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef OPTEE_CLIENT_INTERNAL_H_
#define OPTEE_CLIENT_INTERNAL_H_

#include <Uefi.h>
#include <Library/ArmSmcLib.h>
#include <Library/tee_client_api.h>

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
#define OPTEE_MSG_ATTR_TYPE_RMEM_INPUT    0x5
#define OPTEE_MSG_ATTR_TYPE_RMEM_OUTPUT   0x6
#define OPTEE_MSG_ATTR_TYPE_RMEM_INOUT    0x7
#define OPTEE_MSG_ATTR_TYPE_TMEM_INPUT    0x9
#define OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT   0xa
#define OPTEE_MSG_ATTR_TYPE_TMEM_INOUT    0xb
#define OPTEE_MSG_ATTR_TYPE_MASK          0xff
#define OPTEE_MSG_ATTR_META               BIT8

#define OPTEE_MSG_CMD_OPEN_SESSION    0
#define OPTEE_MSG_CMD_INVOKE_COMMAND  1
#define OPTEE_MSG_CMD_CLOSE_SESSION   2

#define OPTEE_RPC_CMD_LOAD_TA       0
#define OPTEE_RPC_CMD_FS            2
#define OPTEE_RPC_CMD_GET_TIME      3
#define OPTEE_RPC_CMD_NOTIFICATION  4
#define OPTEE_RPC_CMD_SUSPEND       5
#define OPTEE_RPC_CMD_SHM_ALLOC     6
#define OPTEE_RPC_CMD_SHM_FREE      7

#define OPTEE_RPC_FS_OPEN      0
#define OPTEE_RPC_FS_CREATE    1
#define OPTEE_RPC_FS_CLOSE     2
#define OPTEE_RPC_FS_READ      3
#define OPTEE_RPC_FS_WRITE     4
#define OPTEE_RPC_FS_TRUNCATE  5
#define OPTEE_RPC_FS_REMOVE    6
#define OPTEE_RPC_FS_RENAME    7
#define OPTEE_RPC_FS_OPENDIR   8

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
      UINT64    Offs;
      UINT64    Size;
      UINT64    ShmRef;
    } Rmem;
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

//
// Shared memory pool carved out of OP-TEE's reserved non-secure region.
// Physical and virtual addresses are identical in UEFI, so the physical
// address doubles as the shared-memory cookie.
//
EFI_STATUS
OpteeShmInit (
  VOID
  );

VOID *
OpteeShmAlloc (
  IN UINTN  Size,
  IN UINTN  Alignment
  );

VOID
OpteeShmFree (
  IN VOID  *Buffer
  );

BOOLEAN
OpteeShmContains (
  IN UINT64  Address,
  IN UINT64  Size
  );

//
// Resolve a memref parameter of an RPC argument to a pointer and size.
//
BOOLEAN
OpteeMsgParamToBuffer (
  IN  OPTEE_MSG_PARAM  *Param,
  OUT VOID             **Buffer,
  OUT UINTN            *Size
  );

VOID
OpteeMsgParamSetSize (
  IN OUT OPTEE_MSG_PARAM  *Param,
  IN     UINTN            Size
  );

//
// Run a standard call until OP-TEE returns from every RPC.
//
TEEC_Result
OpteeCallWithArg (
  IN OPTEE_MSG_ARG  *Arg
  );

VOID
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
