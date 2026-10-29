/** @file
  Stateful RPC boundary fuzzing with live, owned synthetic shared memory.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <Library/BaseMemoryLib.h>
#include "TestSupport.h"

STATIC EFI_STATUS
FuzzTime (EFI_TIME *Time)
{
  ZeroMem (Time, sizeof (*Time));
  Time->Year = 2026;
  Time->Month = 1;
  Time->Day = 1;
  Time->TimeZone = EFI_UNSPECIFIED_TIMEZONE;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
FuzzFv (CONST EFI_GUID *Guid, VOID **Image, UINTN *Size)
{
  (VOID)Guid; (VOID)Image; (VOID)Size;
  return EFI_NOT_FOUND;
}

int
LLVMFuzzerTestOneInput (const uint8_t *Data, size_t Size)
{
  ARM_SMC_ARGS Args = { 0 };
  OPTEE_MSG_ARG *Arg;
  OPTEE_MSG_ARG Header;
  UINTN Length;
  UINT32 Owner;
  UINT64 Address;
  BOOLEAN Safe;
  UINT8 *Client;
  UINT8 *Payload;
  UINTN Index;
  UINTN TimeCalls;
  UINTN FvCalls;
  OPTEE_MSG_PARAM *Param;
  VOID *Allocated;
  UINT64 Storage[OPTEE_MSG_ARG_SIZE (OPTEE_RPC_MAX_PARAMS) / sizeof (UINT64)];
  BOOLEAN PayloadFreed;

  if (Size < 2) {
    return 0;
  }

  TestInit ();
  TestGetTimeHook = FuzzTime;
  TestFvHook = FuzzFv;
  Length = sizeof (OPTEE_MSG_ARG) + Data[0] % (OPTEE_RPC_MAX_PARAMS * sizeof (OPTEE_MSG_PARAM) + 1);
  Owner = (Data[1] & 1) ? OPTEE_SHM_RPC_ARG : OPTEE_SHM_CLIENT;
  Arg = OpteeShmAllocOwned (Length, 8, Owner);
  Client = OpteeShmAlloc (32, 8);
  Payload = OpteeShmAllocOwned (32, 8, OPTEE_SHM_RPC_APPL);
  assert (Arg != NULL && Client != NULL && Payload != NULL);
  SetMem (Client, 32, 0x5a);
  SetMem (Payload, 32, 0xa5);
  CopyMem (Arg, Data + 2, MIN (Size - 2, Length));
  // Keep the raw-byte boundary and also reach valid command/type combinations.
  if ((Data[1] & 8) != 0 && Length >= OPTEE_MSG_ARG_SIZE (1)) {
    Arg->Cmd = Data[0] % 9;
    Arg->NumParams = (UINT32)((Length - sizeof (*Arg)) / sizeof (OPTEE_MSG_PARAM));
    for (Index = 0; Index < Arg->NumParams; Index++) {
      Param = &Arg->Params[Index];
      if ((Param->Attr & 1) != 0) {
        Param->Attr = OPTEE_MSG_ATTR_TYPE_VALUE_INPUT + (Data[Index % Size] % 3);
      } else {
        Param->Attr = OPTEE_MSG_ATTR_TYPE_TMEM_INPUT + (Data[Index % Size] % 3);
        Param->U.Tmem.BufPtr = (UINTN)((Data[0] & 1) ? Payload : Client);
        Param->U.Tmem.ShmRef = Param->U.Tmem.BufPtr;
        Param->U.Tmem.Size %= 33;
      }
    }
  }
  CopyMem (&Header, Arg, sizeof (Header));
  PayloadFreed = (Header.Cmd == OPTEE_RPC_CMD_SHM_FREE) &&
                 (Header.NumParams == 1) && (Length >= OPTEE_MSG_ARG_SIZE (1)) &&
                 (Arg->Params[0].Attr == OPTEE_MSG_ATTR_TYPE_VALUE_INPUT) &&
                 (Arg->Params[0].U.Value.A == 0) &&
                 (Arg->Params[0].U.Value.B == (UINTN)Payload);
  Address = (UINT64)(UINTN)Arg;
  if ((Data[1] & 2) != 0) {
    Address++;
  }

  Args.Arg0 = OPTEE_SMC_RETURN_RPC_PREFIX | OPTEE_SMC_RPC_FUNC_CMD;
  if ((Data[1] & 4) != 0) {
    Args.Arg0 = OPTEE_SMC_RETURN_RPC_PREFIX | 0x1234;
  }
  Args.Arg1 = Address >> 32;
  Args.Arg2 = (UINT32)Address;
  Args.Arg3 = 0x12345678;
  Args.Arg6 = 0xabcdef;
  Args.Arg7 = 0x987654;
  TimeCalls = TestTimeCalls;
  FvCalls = TestFvCalls;
  Safe = OpteeRpcHandle (&Args);
  assert (Args.Arg3 == 0x12345678 && Args.Arg6 == 0xabcdef && Args.Arg7 == 0x987654);
  if ((Data[1] & 7) != 1 || Header.NumParams > OPTEE_RPC_MAX_PARAMS ||
      OPTEE_MSG_ARG_SIZE (Header.NumParams) > Length) {
    assert (!Safe);
    assert (TestTimeCalls == TimeCalls && TestFvCalls == FvCalls);
  } else {
    assert (Safe);
    assert (Arg->NumParams == Header.NumParams);
    assert (Arg->Cmd == Header.Cmd);
    if (Header.Cmd > OPTEE_RPC_CMD_SHM_FREE || Header.Cmd == 1) {
      assert (Arg->Ret == TEEC_ERROR_NOT_SUPPORTED);
    }
  }

  /* Neither dispatcher nor rejected memrefs may reach client-owned bytes. */
  for (Index = 0; Index < 32; Index++) {
    assert (Client[Index] == 0x5a);
  }
  assert (OpteeShmIsOwned ((UINT64)(UINTN)Client, 32, OPTEE_SHM_CLIENT));
  if (Safe && PayloadFreed) {
    assert (Arg->Ret == TEEC_SUCCESS);
    assert (!OpteeShmContains ((UINTN)Payload, 1));
  } else {
    assert (OpteeShmIsOwned ((UINT64)(UINTN)Payload, 32, OPTEE_SHM_RPC_APPL));
  }
  // A second transaction checks real allocate/free ownership and zeroing.
  Arg = OpteeShmAllocOwned (sizeof (Storage), 8, OPTEE_SHM_RPC_ARG);
  assert (Arg != NULL);
  Arg->Cmd = OPTEE_RPC_CMD_SHM_ALLOC;
  Arg->NumParams = 1;
  Arg->Params[0].Attr = OPTEE_MSG_ATTR_TYPE_VALUE_INPUT;
  Arg->Params[0].U.Value.A = Data[0] % 3;
  Arg->Params[0].U.Value.B = Data[1] + 1;
  Arg->Params[0].U.Value.C = 8;
  Address = (UINTN)Arg;
  Args.Arg0 = OPTEE_SMC_RETURN_RPC_PREFIX | OPTEE_SMC_RPC_FUNC_CMD;
  Args.Arg1 = Address >> 32;
  Args.Arg2 = (UINT32)Address;
  assert (OpteeRpcHandle (&Args));
  assert (Arg->Ret == TEEC_SUCCESS);
  Allocated = (VOID *)(UINTN)Arg->Params[0].U.Tmem.BufPtr;
  Owner = OPTEE_SHM_RPC_APPL + Data[0] % 3;
  assert (OpteeShmIsOwned ((UINTN)Allocated, Data[1] + 1, Owner));
  for (Index = 0; Index < Data[1] + 1U; Index++) {
    assert (((UINT8 *)Allocated)[Index] == 0);
  }
  Arg->Cmd = OPTEE_RPC_CMD_SHM_FREE;
  Arg->Params[0].Attr = OPTEE_MSG_ATTR_TYPE_VALUE_INPUT;
  Arg->Params[0].U.Value.A = Data[0] % 3;
  Arg->Params[0].U.Value.B = (UINTN)Allocated;
  assert (OpteeRpcHandle (&Args) && Arg->Ret == TEEC_SUCCESS);
  assert (!OpteeShmContains ((UINTN)Allocated, 1));
  assert (OpteeRpcHandle (&Args) && Arg->Ret == TEEC_ERROR_BAD_PARAMETERS);
  TestFini ();
  return 0;
}
