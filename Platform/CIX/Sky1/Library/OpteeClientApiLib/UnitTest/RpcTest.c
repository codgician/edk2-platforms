/** @file
  RPC boundary regressions against the real dispatcher.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include "TestSupport.h"

STATIC OPTEE_MSG_ARG *mShared;

STATIC OPTEE_MSG_ARG *
RpcArgument (UINTN Size)
{
  ARM_SMC_ARGS Args = { 0 };
  Args.Arg0 = OPTEE_SMC_RETURN_RPC_PREFIX | OPTEE_SMC_RPC_FUNC_ALLOC;
  Args.Arg1 = Size;
  OpteeRpcHandle (&Args);
  mShared = (VOID *)(UINTN)(((UINT64)Args.Arg1 << 32) | (UINT32)Args.Arg2);
  assert_non_null (mShared);
  ZeroMem (mShared, Size);
  return mShared;
}

STATIC BOOLEAN
Dispatch (OPTEE_MSG_ARG *Arg)
{
  ARM_SMC_ARGS Args = { 0 };
  UINT64 Address = (UINT64)(UINTN)Arg;
  BOOLEAN Safe;
  Args.Arg0 = OPTEE_SMC_RETURN_RPC_PREFIX | OPTEE_SMC_RPC_FUNC_CMD;
  Args.Arg1 = Address >> 32;
  Args.Arg2 = (UINT32)Address;
  Args.Arg3 = 0x13579;
  Safe = OpteeRpcHandle (&Args);
  assert_int_equal (Args.Arg3, 0x13579);
  return Safe;
}

STATIC EFI_STATUS
GoodTime (EFI_TIME *Time)
{
  ZeroMem (Time, sizeof (*Time));
  Time->Year = 2026;
  Time->Month = 1;
  Time->Day = 1;
  Time->TimeZone = EFI_UNSPECIFIED_TIMEZONE;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
MissingImage (CONST EFI_GUID *Guid, VOID **Image, UINTN *Size)
{
  (VOID)Guid; (VOID)Image; (VOID)Size;
  return EFI_NOT_FOUND;
}

STATIC VOID
WrongDirectionTime (VOID **State)
{
  OPTEE_MSG_ARG *Arg;
  (VOID)State;
  TestInit ();
  TestGetTimeHook = GoodTime;
  Arg = RpcArgument (OPTEE_MSG_ARG_SIZE (1));
  Arg->Cmd = OPTEE_RPC_CMD_GET_TIME;
  Arg->NumParams = 1;
  Arg->Params[0].Attr = OPTEE_MSG_ATTR_TYPE_VALUE_INPUT;
  Dispatch (Arg);
  assert_int_equal (Arg->Ret, TEEC_ERROR_BAD_PARAMETERS);
  assert_int_equal (TestTimeCalls, 0);
  TestFini ();
}

STATIC VOID
DeniedTa (VOID **State)
{
  OPTEE_MSG_ARG *Arg;
  (VOID)State;
  TestInit ();
  TestFvHook = MissingImage;
  Arg = RpcArgument (OPTEE_MSG_ARG_SIZE (2));
  Arg->Cmd = OPTEE_RPC_CMD_LOAD_TA;
  Arg->NumParams = 2;
  Arg->Params[0].Attr = OPTEE_MSG_ATTR_TYPE_VALUE_INPUT;
  Arg->Params[1].Attr = OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT;
  Dispatch (Arg);
  assert_int_equal (Arg->Ret, TEEC_ERROR_ITEM_NOT_FOUND);
  assert_int_equal (TestFvCalls, 0);
  TestFini ();
}

STATIC VOID
OversizedSuspend (VOID **State)
{
  OPTEE_MSG_ARG *Arg;
  (VOID)State;
  TestInit ();
  Arg = RpcArgument (OPTEE_MSG_ARG_SIZE (1));
  Arg->Cmd = OPTEE_RPC_CMD_SUSPEND;
  Arg->NumParams = 1;
  Arg->Params[0].Attr = OPTEE_MSG_ATTR_TYPE_VALUE_INPUT;
  Arg->Params[0].U.Value.A = 101;
  Dispatch (Arg);
  assert_int_equal (Arg->Ret, TEEC_ERROR_BAD_PARAMETERS);
  assert_int_equal (TestDelayCalls, 0);
  TestFini ();
}

STATIC VOID
TruncatedArgument (VOID **State)
{
  OPTEE_MSG_ARG *Arg;
  (VOID)State;
  TestInit ();
  TestGetTimeHook = GoodTime;
  Arg = RpcArgument (sizeof (*Arg));
  Arg->Cmd = OPTEE_RPC_CMD_GET_TIME;
  Arg->NumParams = 1;
  /* Baseline rounds to pages; this write stays in its page but not its request. */
  Arg->Params[0].Attr = OPTEE_MSG_ATTR_TYPE_VALUE_OUTPUT;
  assert_false (Dispatch (Arg));
  assert_int_equal (TestTimeCalls, 0);
  TestFini ();
}

STATIC VOID
NonOwningArgument (VOID **State)
{
  OPTEE_MSG_ARG *Arg;
  (VOID)State;
  TestInit ();
  TestGetTimeHook = GoodTime;
  Arg = OpteeShmAlloc (OPTEE_MSG_ARG_SIZE (1), 8);
  assert_non_null (Arg);
  ZeroMem (Arg, OPTEE_MSG_ARG_SIZE (1));
  Arg->Cmd = OPTEE_RPC_CMD_GET_TIME;
  Arg->NumParams = 1;
  Arg->Params[0].Attr = OPTEE_MSG_ATTR_TYPE_VALUE_OUTPUT;
  assert_false (Dispatch (Arg));
  assert_int_equal (TestTimeCalls, 0);
  TestFini ();
}

STATIC CONST UINT8 mTaOctets[16] = {
  0xbc, 0x50, 0xd9, 0x71, 0xd4, 0xc9, 0x42, 0xc4,
  0x82, 0xcb, 0x34, 0x3f, 0xb7, 0xf3, 0x78, 0x96
};
STATIC BOOLEAN mMutate;
STATIC VOID *mFvImage;
STATIC UINTN mScrubbedImages;

STATIC VOID
CheckFvScrub (VOID *Buffer)
{
  UINTN Index;
  if (Buffer == mFvImage) {
    for (Index = 0; Index < 16; Index++) {
      assert_int_equal (((UINT8 *)Buffer)[Index], 0);
    }
    mFvImage = NULL;
    mScrubbedImages++;
  }
}

STATIC EFI_STATUS
ImageFromFv (CONST EFI_GUID *Guid, VOID **Image, UINTN *Size)
{
  STATIC CONST EFI_GUID Expected = {
    0xbc50d971, 0xd4c9, 0x42c4, {0x82,0xcb,0x34,0x3f,0xb7,0xf3,0x78,0x96}
  };
  assert_memory_equal (Guid, &Expected, sizeof (*Guid));
  *Size = 16;
  *Image = AllocatePool (*Size);
  assert_non_null (*Image);
  mFvImage = *Image;
  SetMem (*Image, *Size, 0x3c);
  if (mMutate) {
    mShared->NumParams = MAX_UINT32;
    mShared->Cmd = OPTEE_RPC_CMD_SHM_FREE;
    mShared->Params[1].U.Tmem.BufPtr = 1;
    mShared->Params[1].Attr = MAX_UINT64;
  }
  return EFI_SUCCESS;
}

STATIC VOID
SetLoad (OPTEE_MSG_ARG *Arg, VOID *Buffer, UINTN Size)
{
  ZeroMem (Arg, OPTEE_MSG_ARG_SIZE (2));
  Arg->Cmd = OPTEE_RPC_CMD_LOAD_TA;
  Arg->NumParams = 2;
  Arg->Params[0].Attr = OPTEE_MSG_ATTR_TYPE_VALUE_INPUT;
  CopyMem (&Arg->Params[0].U.Value, mTaOctets, sizeof (mTaOctets));
  Arg->Params[1].Attr = OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT;
  Arg->Params[1].U.Tmem.BufPtr = (UINTN)Buffer;
  Arg->Params[1].U.Tmem.ShmRef = (UINTN)Buffer;
  Arg->Params[1].U.Tmem.Size = Size;
}

STATIC VOID
AllowedTaAndSnapshot (VOID **State)
{
  OPTEE_MSG_ARG *Arg;
  UINT8 *Buffer;
  UINTN Index;
  (VOID)State;
  TestInit ();
  TestFvHook = ImageFromFv;
  TestFreeHook = CheckFvScrub;
  mFvImage = NULL;
  mScrubbedImages = 0;
  Arg = RpcArgument (OPTEE_MSG_ARG_SIZE (2) + 32);
  SetMem ((UINT8 *)Arg + OPTEE_MSG_ARG_SIZE (2), 32, 0x6d);
  Buffer = OpteeShmAllocOwned (32, 8, OPTEE_SHM_RPC_APPL);
  assert_non_null (Buffer);
  mMutate = FALSE;
  SetLoad (Arg, NULL, 0);
  assert_true (Dispatch (Arg));
  assert_int_equal (Arg->Ret, TEEC_SUCCESS);
  assert_int_equal (Arg->Params[1].U.Tmem.Size, 16);
  SetMem (Buffer, 32, 0x7b);
  SetLoad (Arg, Buffer, 8);
  assert_true (Dispatch (Arg));
  assert_int_equal (Arg->Ret, TEEC_ERROR_SHORT_BUFFER);
  assert_int_equal (Arg->Params[1].U.Tmem.Size, 16);
  for (Index = 0; Index < 32; Index++) { assert_int_equal (Buffer[Index], 0x7b); }
  mMutate = TRUE;
  SetLoad (Arg, Buffer, 32);
  assert_true (Dispatch (Arg));
  assert_int_equal (Arg->Ret, TEEC_SUCCESS);
  assert_int_equal (Arg->NumParams, 2);
  assert_int_equal (Arg->Cmd, OPTEE_RPC_CMD_LOAD_TA);
  assert_int_equal (Arg->Params[1].Attr, OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT);
  assert_int_equal (Arg->Params[1].U.Tmem.BufPtr, (UINTN)Buffer);
  for (Index = 0; Index < 32; Index++) {
    assert_int_equal (((UINT8 *)Arg)[OPTEE_MSG_ARG_SIZE (2) + Index], 0x6d);
  }
  for (Index = 0; Index < 16; Index++) { assert_int_equal (Buffer[Index], 0x3c); }
  for (; Index < 32; Index++) { assert_int_equal (Buffer[Index], 0x7b); }
  assert_int_equal (TestFvCalls, 3);
  assert_int_equal (mScrubbedImages, 3);
  mMutate = FALSE;
  TestFvHook = MissingImage;
  SetLoad (Arg, NULL, 0);
  assert_true (Dispatch (Arg));
  assert_int_equal (Arg->Ret, TEEC_ERROR_ITEM_NOT_FOUND);
  assert_int_equal (TestFvCalls, 4);
  TestFini ();
}

STATIC EFI_STATUS
MutatingTime (EFI_TIME *Time)
{
  mShared->NumParams = MAX_UINT32;
  mShared->Cmd = OPTEE_RPC_CMD_LOAD_TA;
  mShared->Params[0].Attr = MAX_UINT64;
  return GoodTime (Time);
}

STATIC EFI_STATUS
BadTime (EFI_TIME *Time)
{
  GoodTime (Time);
  Time->Month = 2;
  Time->Day = 30;
  return EFI_SUCCESS;
}

STATIC VOID
TimeSnapshotAndValidation (VOID **State)
{
  OPTEE_MSG_ARG *Arg;
  UINTN Index;
  (VOID)State;
  TestInit ();
  Arg = RpcArgument (OPTEE_MSG_ARG_SIZE (1) + 32);
  SetMem ((UINT8 *)Arg + OPTEE_MSG_ARG_SIZE (1), 32, 0x6d);
  Arg->Cmd = OPTEE_RPC_CMD_GET_TIME;
  Arg->NumParams = 1;
  Arg->Params[0].Attr = OPTEE_MSG_ATTR_TYPE_VALUE_OUTPUT;
  TestGetTimeHook = MutatingTime;
  assert_true (Dispatch (Arg));
  assert_int_equal (Arg->Ret, TEEC_SUCCESS);
  assert_int_equal (Arg->NumParams, 1);
  assert_int_equal (Arg->Cmd, OPTEE_RPC_CMD_GET_TIME);
  assert_int_equal (Arg->Params[0].Attr, OPTEE_MSG_ATTR_TYPE_VALUE_OUTPUT);
  assert_int_equal (Arg->Params[0].U.Value.A, 1767225600);
  assert_int_equal (Arg->Params[0].U.Value.B, 0);
  for (Index = 0; Index < 32; Index++) {
    assert_int_equal (((UINT8 *)Arg)[OPTEE_MSG_ARG_SIZE (1) + Index], 0x6d);
  }
  TestGetTimeHook = BadTime;
  assert_true (Dispatch (Arg));
  assert_int_equal (Arg->Ret, TEEC_ERROR_GENERIC);
  Arg->Params[0].Attr = OPTEE_MSG_ATTR_TYPE_VALUE_OUTPUT | BIT8;
  assert_true (Dispatch (Arg));
  assert_int_equal (Arg->Ret, TEEC_ERROR_BAD_PARAMETERS);
  assert_int_equal (TestTimeCalls, 2);
  TestFini ();
}

STATIC VOID
CountsUnknownAndNotifications (VOID **State)
{
  OPTEE_MSG_ARG *Arg;
  ARM_SMC_ARGS Args = { 0 };
  (VOID)State;
  TestInit ();
  Arg = RpcArgument (OPTEE_MSG_ARG_SIZE (4));
  Arg->Cmd = 0x1234;
  Arg->NumParams = 4;
  assert_true (Dispatch (Arg));
  assert_int_equal (Arg->Ret, TEEC_ERROR_NOT_SUPPORTED);
  Arg->NumParams = 5;
  assert_false (Dispatch (Arg));
  Arg->NumParams = MAX_UINT32;
  assert_false (Dispatch (Arg));
  Arg->NumParams = 0;
  assert_true (Dispatch (Arg));
  assert_int_equal (Arg->Ret, TEEC_ERROR_NOT_SUPPORTED);
  Arg->Cmd = OPTEE_RPC_CMD_NOTIFICATION;
  Arg->NumParams = 1;
  Arg->Params[0].Attr = OPTEE_MSG_ATTR_TYPE_VALUE_INPUT;
  Arg->Params[0].U.Value.A = 0;
  assert_true (Dispatch (Arg));
  assert_int_equal (Arg->Ret, TEEC_ERROR_NOT_SUPPORTED);
  Arg->Params[0].U.Value.A = 1;
  assert_true (Dispatch (Arg));
  assert_int_equal (Arg->Ret, TEEC_SUCCESS);
  Arg->Params[0].U.Value.A = 2;
  assert_true (Dispatch (Arg));
  assert_int_equal (Arg->Ret, TEEC_ERROR_BAD_PARAMETERS);
  Args.Arg0 = OPTEE_SMC_RETURN_RPC_PREFIX | 0x1234;
  Args.Arg3 = 0xabc;
  assert_false (OpteeRpcHandle (&Args));
  assert_int_equal (Args.Arg3, 0xabc);
  assert_int_equal (TestTimeCalls + TestFvCalls + TestDelayCalls, 0);
  TestFini ();
}

STATIC VOID
PayloadOwnership (VOID **State)
{
  OPTEE_MSG_ARG *Arg;
  VOID *Payload;
  UINTN Type;
  UINT32 Owners[] = { OPTEE_SHM_RPC_APPL, OPTEE_SHM_RPC_KERNEL, OPTEE_SHM_RPC_GLOBAL };
  (VOID)State;
  TestInit ();
  Arg = RpcArgument (OPTEE_MSG_ARG_SIZE (1));
  for (Type = 0; Type < 3; Type++) {
    ZeroMem (Arg, OPTEE_MSG_ARG_SIZE (1));
    Arg->Cmd = OPTEE_RPC_CMD_SHM_ALLOC;
    Arg->NumParams = 1;
    Arg->Params[0].Attr = OPTEE_MSG_ATTR_TYPE_VALUE_INPUT;
    Arg->Params[0].U.Value.A = Type;
    Arg->Params[0].U.Value.B = 32;
    Arg->Params[0].U.Value.C = 8;
    assert_true (Dispatch (Arg));
    assert_int_equal (Arg->Ret, TEEC_SUCCESS);
    Payload = (VOID *)(UINTN)Arg->Params[0].U.Tmem.BufPtr;
    assert_true (OpteeShmIsOwned ((UINTN)Payload, 32, Owners[Type]));
    assert_int_equal (Arg->Params[0].Attr, OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT);
    Arg->Cmd = OPTEE_RPC_CMD_SHM_FREE;
    Arg->Params[0].Attr = OPTEE_MSG_ATTR_TYPE_VALUE_INPUT;
    Arg->Params[0].U.Value.A = (Type + 1) % 3;
    Arg->Params[0].U.Value.B = (UINTN)Payload;
    assert_true (Dispatch (Arg));
    assert_int_equal (Arg->Ret, TEEC_ERROR_BAD_PARAMETERS);
    assert_true (OpteeShmIsOwned ((UINTN)Payload, 32, Owners[Type]));
    Arg->Params[0].U.Value.A = Type;
    Arg->Params[0].U.Value.B++;
    assert_true (Dispatch (Arg));
    assert_int_equal (Arg->Ret, TEEC_ERROR_BAD_PARAMETERS);
    Arg->Params[0].U.Value.B--;
    assert_true (Dispatch (Arg));
    assert_int_equal (Arg->Ret, TEEC_SUCCESS);
    assert_false (OpteeShmContains ((UINTN)Payload, 1));
    assert_true (Dispatch (Arg));
    assert_int_equal (Arg->Ret, TEEC_ERROR_BAD_PARAMETERS);
  }
  Arg->Cmd = OPTEE_RPC_CMD_SHM_FREE;
  Arg->Params[0].U.Value.A = 0;
  Arg->Params[0].U.Value.B = (UINTN)Arg;
  assert_true (Dispatch (Arg));
  assert_int_equal (Arg->Ret, TEEC_ERROR_BAD_PARAMETERS);
  Payload = OpteeShmAlloc (32, 8);
  assert_non_null (Payload);
  Arg->Params[0].U.Value.B = (UINTN)Payload;
  assert_true (Dispatch (Arg));
  assert_int_equal (Arg->Ret, TEEC_ERROR_BAD_PARAMETERS);
  assert_true (OpteeShmIsOwned ((UINTN)Payload, 32, OPTEE_SHM_CLIENT));
  Arg->Cmd = OPTEE_RPC_CMD_SHM_ALLOC;
  Arg->Params[0].U.Value.A = 3;
  Arg->Params[0].U.Value.B = 32;
  Arg->Params[0].U.Value.C = 8;
  assert_true (Dispatch (Arg));
  assert_int_equal (Arg->Ret, TEEC_ERROR_BAD_PARAMETERS);
  Arg->Params[0].U.Value.A = 0;
  Arg->Params[0].U.Value.C = 3;
  assert_true (Dispatch (Arg));
  assert_int_equal (Arg->Ret, TEEC_ERROR_BAD_PARAMETERS);
  Arg->Params[0].U.Value.C = 8;
  Arg->Params[0].U.Value.B = TEST_POOL_SIZE;
  assert_true (Dispatch (Arg));
  assert_int_equal (Arg->Ret, TEEC_ERROR_OUT_OF_MEMORY);
  TestFini ();
}

STATIC VOID
RejectArgumentAndClientMemrefs (VOID **State)
{
  OPTEE_MSG_ARG *Arg;
  VOID *Client;
  (VOID)State;
  TestInit ();
  Arg = RpcArgument (OPTEE_MSG_ARG_SIZE (2));
  Client = OpteeShmAlloc (32, 8);
  assert_non_null (Client);
  SetLoad (Arg, Client, 32);
  assert_true (Dispatch (Arg));
  assert_int_equal (Arg->Ret, TEEC_ERROR_BAD_PARAMETERS);
  SetLoad (Arg, Arg, sizeof (*Arg));
  assert_true (Dispatch (Arg));
  assert_int_equal (Arg->Ret, TEEC_ERROR_BAD_PARAMETERS);
  SetLoad (Arg, NULL, 0);
  Arg->Params[1].Attr = OPTEE_MSG_ATTR_TYPE_TMEM_INPUT;
  assert_true (Dispatch (Arg));
  assert_int_equal (Arg->Ret, TEEC_ERROR_BAD_PARAMETERS);
  SetLoad (Arg, NULL, 0);
  Arg->Params[1].Attr |= BIT8;
  assert_true (Dispatch (Arg));
  assert_int_equal (Arg->Ret, TEEC_ERROR_BAD_PARAMETERS);
  SetLoad (Arg, NULL, 0);
  Arg->Params[1].Attr = OPTEE_MSG_ATTR_TYPE_RMEM_OUTPUT;
  assert_true (Dispatch (Arg));
  assert_int_equal (Arg->Ret, TEEC_ERROR_BAD_PARAMETERS);
  SetLoad (Arg, NULL, 0);
  Arg->Params[0].Attr = OPTEE_MSG_ATTR_TYPE_VALUE_INOUT;
  assert_true (Dispatch (Arg));
  assert_int_equal (Arg->Ret, TEEC_ERROR_BAD_PARAMETERS);
  assert_int_equal (TestFvCalls, 0);
  TestFini ();
}

STATIC VOID
SmcAllocationAndFree (VOID **State)
{
  ARM_SMC_ARGS Args = { 0 };
  VOID *Buffer;
  UINT64 Address;
  (VOID)State;
  TestInit ();
  Args.Arg0 = OPTEE_SMC_RETURN_RPC_PREFIX | OPTEE_SMC_RPC_FUNC_ALLOC;
  Args.Arg1 = 64;
  Args.Arg3 = 0x333;
  Args.Arg6 = 0x666;
  Args.Arg7 = 0x777;
  assert_true (OpteeRpcHandle (&Args));
  assert_int_equal (Args.Arg1, Args.Arg4);
  assert_int_equal (Args.Arg2, Args.Arg5);
  Address = ((UINT64)Args.Arg1 << 32) | Args.Arg2;
  Buffer = (VOID *)(UINTN)Address;
  assert_true (OpteeShmIsOwned (Address, 64, OPTEE_SHM_RPC_ARG));
  assert_int_equal (Args.Arg3, 0x333);
  assert_int_equal (Args.Arg6, 0x666);
  assert_int_equal (Args.Arg7, 0x777);
  Args.Arg0 = OPTEE_SMC_RETURN_RPC_PREFIX | OPTEE_SMC_RPC_FUNC_FREE;
  Args.Arg2++;
  assert_false (OpteeRpcHandle (&Args));
  assert_true (OpteeShmIsOwned (Address, 64, OPTEE_SHM_RPC_ARG));
  Args.Arg2--;
  assert_true (OpteeRpcHandle (&Args));
  assert_false (OpteeShmContains ((UINTN)Buffer, 1));
  assert_false (OpteeRpcHandle (&Args));
  Args.Arg0 = OPTEE_SMC_RETURN_RPC_PREFIX | OPTEE_SMC_RPC_FUNC_ALLOC;
  Args.Arg1 = TEST_POOL_SIZE + 1;
  assert_true (OpteeRpcHandle (&Args));
  assert_int_equal (Args.Arg1 | Args.Arg2 | Args.Arg4 | Args.Arg5, 0);
  Args.Arg0 = OPTEE_SMC_RETURN_RPC_PREFIX | OPTEE_SMC_RPC_FUNC_FOREIGN_INTR;
  assert_true (OpteeRpcHandle (&Args));
  assert_int_equal (Args.Arg3, 0x333);
  assert_int_equal (Args.Arg6, 0x666);
  assert_int_equal (Args.Arg7, 0x777);
  TestFini ();
}

STATIC VOID
SuspendBounds (VOID **State)
{
  OPTEE_MSG_ARG *Arg;
  (VOID)State;
  TestInit ();
  Arg = RpcArgument (OPTEE_MSG_ARG_SIZE (1));
  Arg->Cmd = OPTEE_RPC_CMD_SUSPEND;
  Arg->NumParams = 1;
  Arg->Params[0].Attr = OPTEE_MSG_ATTR_TYPE_VALUE_INPUT;
  Arg->Params[0].U.Value.A = 100;
  assert_true (Dispatch (Arg));
  assert_int_equal (Arg->Ret, TEEC_SUCCESS);
  assert_int_equal (TestDelayCalls, 1);
  assert_int_equal (TestDelayMicroseconds, 100000);
  Arg->Params[0].U.Value.A = MAX_UINT64;
  assert_true (Dispatch (Arg));
  assert_int_equal (Arg->Ret, TEEC_ERROR_BAD_PARAMETERS);
  Arg->Params[0].U.Value.A = 1;
  Arg->Params[0].Attr = OPTEE_MSG_ATTR_TYPE_VALUE_OUTPUT;
  assert_true (Dispatch (Arg));
  assert_int_equal (Arg->Ret, TEEC_ERROR_BAD_PARAMETERS);
  assert_int_equal (TestDelayCalls, 1);
  Arg->Params[0].Attr = OPTEE_MSG_ATTR_TYPE_VALUE_INPUT;
  Arg->Params[0].U.Value.A = 100;
  for (UINTN Index = 1; Index < 10; Index++) {
    assert_true (Dispatch (Arg));
    assert_int_equal (Arg->Ret, TEEC_SUCCESS);
  }
  assert_int_equal (TestDelayCalls, 10);
  assert_int_equal (TestDelayMicroseconds, 1000000);
  Arg->Params[0].U.Value.A = 1;
  assert_true (Dispatch (Arg));
  assert_int_equal (Arg->Ret, TEEC_ERROR_BAD_PARAMETERS);
  assert_int_equal (TestDelayCalls, 10);
  TestFini ();
}

STATIC EFI_STATUS
FailedTime (EFI_TIME *Time)
{
  (VOID)Time;
  return EFI_DEVICE_ERROR;
}

STATIC EFI_STATUS
NoMemoryImage (CONST EFI_GUID *Guid, VOID **Image, UINTN *Size)
{
  (VOID)Guid;
  *Size = 16;
  *Image = AllocatePool (*Size);
  assert_null (*Image);
  return EFI_OUT_OF_RESOURCES;
}

STATIC VOID
ShortHeadersAndServiceErrors (VOID **State)
{
  OPTEE_MSG_ARG *Arg;
  (VOID)State;
  TestInit ();
  Arg = RpcArgument (sizeof (*Arg) - 1);
  assert_false (Dispatch (Arg));
  assert_int_equal (TestTimeCalls + TestFvCalls + TestDelayCalls, 0);
  Arg = RpcArgument (OPTEE_MSG_ARG_SIZE (2));
  Arg->Cmd = OPTEE_RPC_CMD_GET_TIME;
  Arg->NumParams = 1;
  Arg->Params[0].Attr = OPTEE_MSG_ATTR_TYPE_VALUE_OUTPUT;
  TestGetTimeHook = FailedTime;
  assert_true (Dispatch (Arg));
  assert_int_equal (Arg->Ret, TEEC_ERROR_GENERIC);
  assert_int_equal (TestTimeCalls, 1);
  SetLoad (Arg, NULL, 0);
  TestFvHook = NoMemoryImage;
  TestFailAllocation (1);
  assert_true (Dispatch (Arg));
  assert_int_equal (Arg->Ret, TEEC_ERROR_ITEM_NOT_FOUND);
  assert_int_equal (TestFvCalls, 1);
  TestFailAllocation (0);
  TestFini ();
}

int main (void)
{
  const struct CMUnitTest Tests[] = {
    cmocka_unit_test (WrongDirectionTime),
    cmocka_unit_test (DeniedTa),
    cmocka_unit_test (OversizedSuspend),
    cmocka_unit_test (TruncatedArgument),
    cmocka_unit_test (NonOwningArgument),
    cmocka_unit_test (AllowedTaAndSnapshot),
    cmocka_unit_test (TimeSnapshotAndValidation),
    cmocka_unit_test (CountsUnknownAndNotifications),
    cmocka_unit_test (PayloadOwnership),
    cmocka_unit_test (RejectArgumentAndClientMemrefs),
    cmocka_unit_test (SmcAllocationAndFree),
    cmocka_unit_test (ShortHeadersAndServiceErrors),
    cmocka_unit_test (SuspendBounds)
  };
  return cmocka_run_group_tests (Tests, NULL, NULL);
}
