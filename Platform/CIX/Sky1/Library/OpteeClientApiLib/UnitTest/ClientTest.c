/** @file
  Consumer-visible shared-memory, response and session regressions.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include "TestSupport.h"

STATIC UINTN mResponseSize;
STATIC UINT32 mResponseResult;
STATIC UINTN mMutation;
STATIC UINT8 *mBounce;
STATIC BOOLEAN mInOut;
STATIC UINTN mSmcCalls;
STATIC UINTN mRpcStep;
STATIC OPTEE_MSG_ARG *mRpcArgument;

STATIC VOID Respond (ARM_SMC_ARGS *Args) {
  OPTEE_MSG_ARG *Arg = (VOID *)(UINTN)(((UINT64)Args->Arg1 << 32) | (UINT32)Args->Arg2);
  assert (Args->Arg0 == OPTEE_SMC_CALL_WITH_ARG);
  mSmcCalls++;
  if (Arg->Cmd == OPTEE_MSG_CMD_INVOKE_COMMAND) {
    mBounce = (VOID *)(UINTN)Arg->Params[0].U.Tmem.BufPtr;
    if (mBounce != NULL) {
      assert (Arg->Params[0].U.Tmem.Size == 16);
      assert (Arg->Params[0].Attr == (mInOut ? OPTEE_MSG_ATTR_TYPE_TMEM_INOUT : OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT));
      for (UINTN I = 0; I < 4096; I++) {
        assert (mBounce[I] == (mInOut && I < 16 ? 0xA6 : 0));
      }
      memset (mBounce, 0x5C, 16);
      if (mMutation != 0) {
        CopyMem (mBounce, "SECRET-CANARY", sizeof ("SECRET-CANARY"));
      }
    }
    Arg->Params[0].U.Tmem.Size = mResponseSize;
    switch (mMutation) {
      case 1: Arg->Params[0].Attr = OPTEE_MSG_ATTR_TYPE_TMEM_INPUT; break;
      case 2: Arg->Params[0].U.Tmem.ShmRef++; break;
      case 3: Arg->Params[0].U.Tmem.BufPtr = 1; break;
      case 4: Arg->Session++; break;
      case 5: Arg->NumParams = MAX_UINT32; break;
    }
  } else if (Arg->Cmd == OPTEE_MSG_CMD_OPEN_SESSION) {
    STATIC CONST UINT8 Uuid[] = {0xbc,0x50,0xd9,0x71,0xd4,0xc9,0x42,0xc4,0x82,0xcb,0x34,0x3f,0xb7,0xf3,0x78,0x96};
    assert (Arg->NumParams == 6);
    assert (Arg->Params[0].Attr == (OPTEE_MSG_ATTR_META | OPTEE_MSG_ATTR_TYPE_VALUE_INPUT));
    assert (memcmp (&Arg->Params[0].U.Value, Uuid, 16) == 0);
    assert (Arg->Params[1].U.Value.C == TEEC_LOGIN_PUBLIC);
    Arg->Session = 19;
    if ((mMutation == 6) || (mMutation == 9)) { Arg->Params[2].Attr = OPTEE_MSG_ATTR_TYPE_VALUE_OUTPUT; }
    if (mMutation == 7) { Arg->NumParams = 17; }
  } else if (Arg->Cmd == OPTEE_MSG_CMD_CLOSE_SESSION) {
    if (mMutation == 8) { Arg->NumParams = 17; }
    if (mMutation == 9) {
      TestExpectDiagnostic ("response attribute", OPTEE_MSG_ATTR_TYPE_VALUE_OUTPUT, TEEC_ERROR_COMMUNICATION);
      Arg->Session++;
    }
  }
  Arg->Ret = mResponseResult;
  Arg->RetOrigin = TEEC_ORIGIN_TRUSTED_APP;
  Args->Arg0 = OPTEE_SMC_RETURN_OK;
}

STATIC VOID InvokeSizes (VOID) {
  STATIC CONST UINTN Lengths[] = {0, 1, 9, 10, 16, 17, (UINTN)MAX_UINT32 + 1};
  TEEC_Context Ctx = { 1 };
  TEEC_Session Session = { &Ctx, 7 };
  for (UINTN Type = 0; Type < 2; Type++) {
    for (UINTN Result = 0; Result < 3; Result++) {
      for (UINTN I = 0; I < ARRAY_SIZE (Lengths); I++) {
        TEEC_Operation Op = { 0 };
        UINT8 Out[18];
        memset (Out, 0xA6, sizeof (Out));
        mInOut = Type != 0;
        Op.paramTypes = TEEC_PARAM_TYPES (mInOut ? TEEC_MEMREF_TEMP_INOUT : TEEC_MEMREF_TEMP_OUTPUT, 0, 0, 0);
        Op.params[0].tmpref.buffer = Out + 1;
        Op.params[0].tmpref.size = 16;
        TestSmcHook = Respond;
        mResponseSize = Lengths[I];
        mResponseResult = Result == 0 ? TEEC_SUCCESS : (Result == 1 ? TEEC_ERROR_SHORT_BUFFER : TEEC_ERROR_GENERIC);
        TEEC_Result Expected = Result == 0 && Lengths[I] > 16 ? TEEC_ERROR_SHORT_BUFFER : mResponseResult;
        assert (TEEC_InvokeCommand (&Session, 42, &Op, NULL) == Expected);
        if (mResponseResult != TEEC_SUCCESS) {
          TestExpectDiagnostic ("secure result", mResponseResult, mResponseResult);
        } else if (Expected != TEEC_SUCCESS) {
          TestExpectDiagnostic ("response capacity", Lengths[I], TEEC_ERROR_SHORT_BUFFER);
        } else {
          assert (TestDiagnosticCount == 0);
        }
        assert (Op.params[0].tmpref.size == (Result == 2 ? 16 : Lengths[I]));
        for (UINTN J = 0; J < sizeof (Out); J++) {
          assert (Out[J] == (Expected == TEEC_SUCCESS && J > 0 && J <= Lengths[I] ? 0x5C : 0xA6));
        }
        for (UINTN J = 0; J < 4096; J++) { assert (mBounce[J] == 0); }
      }
    }
  }
  mInOut = FALSE;
}

STATIC VOID ConfigFailures (VOID) {
  for (UINTN Case = 0; Case < 12; Case++) {
    TestResetServices ();
    switch (Case) {
      case 0: TestShmBase++; TestShmSize = 1; break;
      case 1: TestShmBase = MAX_UINT64 - 4095; TestShmSize = 8192; break;
      case 2: TestShmSize = MAX_UINT64; break;
      case 3: TestShmSize += 4096; break;
      case 4: TestShmBase -= 4096; break;
      case 5: TestShmFlags = 3; break;
      case 6: TestShmSize = 0; break;
      case 7: TestFailAllocation (1); break;
      case 8: TestMapStatus = EFI_DEVICE_ERROR; break;
      case 9: TestCapabilityStatus = OPTEE_SMC_RETURN_UNKNOWN_FUNCTION; break;
      case 10: TestCapabilityFlags = 0; break;
      case 11: TestConfigStatus = OPTEE_SMC_RETURN_UNKNOWN_FUNCTION; break;
    }
    assert (EFI_ERROR (OpteeClientApiInitialize (NULL)));
    switch (Case) {
      case 0: case 1: case 4: TestExpectDiagnostic ("SHM base", TestShmBase, EFI_UNSUPPORTED); break;
      case 2: case 3: case 6: TestExpectDiagnostic ("SHM size", TestShmSize, EFI_UNSUPPORTED); break;
      case 5: TestExpectDiagnostic ("SHM cache mode", 3, EFI_UNSUPPORTED); break;
      case 7: TestExpectDiagnostic ("SHM records allocation", TEST_POOL_SIZE / 4096, EFI_OUT_OF_RESOURCES); break;
      case 8: TestExpectDiagnostic ("SHM mapping", TEST_POOL_SIZE, EFI_DEVICE_ERROR); break;
      case 9: TestExpectDiagnostic ("SHM capabilities", TestCapabilityStatus, EFI_UNSUPPORTED); break;
      case 10: TestExpectDiagnostic ("SHM capabilities", 0, EFI_UNSUPPORTED); break;
      case 11: TestExpectDiagnostic ("SHM configuration status", TestConfigStatus, EFI_UNSUPPORTED); break;
    }
    assert (TestMapCalls == (Case == 8 ? 1 : 0));
    assert (HostLivePoolAllocations () == 0);
    assert (OpteeClientApiFinalize ());
  }
  TestInit ();
  TestFini ();
}

STATIC VOID RpcConversation (ARM_SMC_ARGS *Args) {
  if (mRpcStep == 0) {
    assert (Args->Arg0 == OPTEE_SMC_CALL_WITH_ARG);
    Args->Arg0 = OPTEE_SMC_RETURN_RPC_PREFIX | OPTEE_SMC_RPC_FUNC_ALLOC;
    Args->Arg1 = OPTEE_MSG_ARG_SIZE (1);
    Args->Arg3 = 0x12345678;
    Args->Arg6 = 0xABCDEF;
    Args->Arg7 = 0x13579;
  } else {
    assert (Args->Arg0 == OPTEE_SMC_RETURN_FROM_RPC);
    assert (Args->Arg3 == 0x12345678 && Args->Arg6 == 0xABCDEF && Args->Arg7 == 0x13579);
    if (mRpcStep == 1) {
      mRpcArgument = (VOID *)(UINTN)(((UINT64)Args->Arg1 << 32) | (UINT32)Args->Arg2);
      assert (OpteeShmIsOwned ((UINTN)mRpcArgument, OPTEE_MSG_ARG_SIZE (1), OPTEE_SHM_RPC_ARG));
      mRpcArgument->Cmd = OPTEE_RPC_CMD_SUSPEND;
      mRpcArgument->NumParams = 1;
      mRpcArgument->Params[0].Attr = OPTEE_MSG_ATTR_TYPE_VALUE_INPUT;
      mRpcArgument->Params[0].U.Value.A = 1;
      Args->Arg0 = OPTEE_SMC_RETURN_RPC_PREFIX | OPTEE_SMC_RPC_FUNC_CMD;
    } else if (mRpcStep == 2) {
      assert (mRpcArgument->Ret == TEEC_SUCCESS);
      Args->Arg0 = OPTEE_SMC_RETURN_RPC_PREFIX | OPTEE_SMC_RPC_FUNC_FREE;
    } else {
      assert (!OpteeShmContains ((UINTN)mRpcArgument, 1));
      Args->Arg0 = OPTEE_SMC_RETURN_OK;
    }
  }
  mRpcStep++;
}

STATIC VOID EndlessRpc (ARM_SMC_ARGS *Args) {
  mSmcCalls++;
  if (mSmcCalls == 1) {
    Args->Arg0 = OPTEE_SMC_RETURN_RPC_PREFIX | OPTEE_SMC_RPC_FUNC_ALLOC;
    Args->Arg1 = OPTEE_MSG_ARG_SIZE (1);
  } else {
    Args->Arg0 = OPTEE_SMC_RETURN_RPC_PREFIX | OPTEE_SMC_RPC_FUNC_FOREIGN_INTR;
  }
}

STATIC UINTN mCacheCalls;
STATIC BOOLEAN mCacheBad;
STATIC BOOLEAN mCacheBadCookie;
STATIC VOID CachePeer (ARM_SMC_ARGS *Args) {
  assert (Args->Arg0 == OPTEE_SMC_DISABLE_SHM_CACHE);
  mCacheCalls++;
  if (mCacheBad) { Args->Arg0 = 2; return; }
  if (mCacheCalls == 1) {
    Args->Arg0 = OPTEE_SMC_RETURN_OK;
    Args->Arg1 = (UINTN)mRpcArgument >> 32;
    Args->Arg2 = (UINT32)(UINTN)mRpcArgument + (mCacheBadCookie ? 1 : 0);
  } else {
    assert (!OpteeShmContains ((UINTN)mRpcArgument, 1));
    for (UINTN I = 0; I < 4096; I++) { assert (((UINT8 *)mRpcArgument)[I] == 0); }
    Args->Arg0 = OPTEE_SMC_RETURN_ENOTAVAIL;
  }
}
STATIC VOID TransportFailure (ARM_SMC_ARGS *Args) {
  mSmcCalls++;
  Args->Arg0 = OPTEE_SMC_RETURN_ETHREAD_LIMIT;
}

int main (int Argc, char **Argv) {
  if (Argc == 1) { ConfigFailures (); return 0; }
  assert (Argc == 2);
  if (strcmp (Argv[1], "config") == 0) { ConfigFailures (); return 0; }
  TestInit ();
  if (strcmp (Argv[1], "zero") == 0) {
    UINT8 *P = OpteeShmAlloc (17, 4096);
    assert (P != NULL);
    for (UINTN I = 0; I < 4096; I++) { assert (P[I] == 0); }
    memset (P, 0x5C, 4096);
    OpteeShmFree (P);
    for (UINTN I = 0; I < 4096; I++) { assert (P[I] == 0); }
  } else if (strcmp (Argv[1], "ownership") == 0) {
    UINT8 *P = OpteeShmAlloc (4097, 4096);
    UINT8 *Q = OpteeShmAlloc (17, 4096);
    assert (P != NULL && Q != NULL && OpteeShmContains ((UINTN)P, 4097));
    assert (!OpteeShmContains ((UINTN)P, 4098));
    assert (!OpteeShmFreeOwned (P + 4096, OPTEE_SHM_CLIENT));
    assert (!OpteeShmFreeOwned (P, OPTEE_SHM_RPC_APPL));
    assert (OpteeShmAlloc (MAX_UINTN, 4096) == NULL);
    assert (OpteeShmAlloc (1, MAX_UINTN) == NULL);
    assert (OpteeShmAlloc (1, 3) == NULL);
    assert (OpteeShmAlloc (1, 0) == NULL);
    OpteeShmFree (P);
    assert (!OpteeShmContains ((UINTN)P, 1));
    assert (OpteeShmIsOwned ((UINTN)Q, 17, OPTEE_SHM_CLIENT));
    assert (!OpteeShmFreeOwned (P, OPTEE_SHM_CLIENT));
  } else if (strcmp (Argv[1], "size") == 0) {
    InvokeSizes ();
  } else if (strcmp (Argv[1], "metadata") == 0) {
    TEEC_Context Ctx = { 1 };
    TEEC_Session Session = { &Ctx, 7 };
    for (mMutation = 1; mMutation <= 5; mMutation++) {
      TEEC_Operation Op = { 0 }; UINT8 Out[16];
      memset (Out, 0xA6, sizeof (Out));
      TestClearDiagnostics ();
      Op.paramTypes = TEEC_PARAM_TYPES (TEEC_MEMREF_TEMP_OUTPUT, 0, 0, 0);
      Op.params[0].tmpref.buffer = Out; Op.params[0].tmpref.size = 16;
      mResponseSize = 10; mResponseResult = TEEC_SUCCESS; TestSmcHook = Respond;
      assert (TEEC_InvokeCommand (&Session, 42, &Op, NULL) == TEEC_ERROR_COMMUNICATION);
      switch (mMutation) {
        case 1: TestExpectDiagnostic ("response attribute", OPTEE_MSG_ATTR_TYPE_TMEM_INPUT, TEEC_ERROR_COMMUNICATION); break;
        case 2: TestExpectDiagnostic ("response memref", (UINTN)mBounce + 1, TEEC_ERROR_COMMUNICATION); break;
        case 3: TestExpectDiagnostic ("response memref", 1, TEEC_ERROR_COMMUNICATION); break;
        case 4: TestExpectDiagnostic ("invoke response framing", 8, TEEC_ERROR_COMMUNICATION); break;
        case 5: TestExpectDiagnostic ("invoke response framing", MAX_UINT32, TEEC_ERROR_COMMUNICATION); break;
      }
      assert (Op.params[0].tmpref.size == 16);
      for (UINTN J = 0; J < sizeof (Out); J++) { assert (Out[J] == 0xA6); }
      for (UINTN J = 0; J < 4096; J++) { assert (mBounce[J] == 0); }
    }
  } else if (strcmp (Argv[1], "close") == 0) {
    TEEC_Context Ctx = { 1 }; TEEC_Session Session = { &Ctx, 7 };
    TestSmcHook = Respond; mResponseResult = TEEC_ERROR_GENERIC;
    assert (OpteeClientCloseSession (&Session) == TEEC_ERROR_GENERIC);
    TestExpectDiagnostic ("close secure result", TEEC_ERROR_GENERIC, TEEC_ERROR_GENERIC);
    assert (Session.ctx == &Ctx && Session.session_id == 7);
    VOID *All = OpteeShmAlloc (TEST_POOL_SIZE, 4096); assert (All != NULL);
    assert (OpteeClientCloseSession (&Session) == TEEC_ERROR_OUT_OF_MEMORY);
    assert (Session.ctx == &Ctx && Session.session_id == 7);
    OpteeShmFree (All);
    mResponseResult = TEEC_SUCCESS;
    mMutation = 8;
    assert (OpteeClientCloseSession (&Session) == TEEC_ERROR_COMMUNICATION);
    TestExpectDiagnostic ("close response framing", 17, TEEC_ERROR_COMMUNICATION);
    assert (Session.ctx == &Ctx && Session.session_id == 7);
    mMutation = 0;
    assert (OpteeClientCloseSession (&Session) == TEEC_SUCCESS);
    assert (Session.ctx == NULL && Session.session_id == 0);
  } else if (strcmp (Argv[1], "open") == 0) {
    TEEC_Context Ctx = { 1 }; TEEC_Session Session = { NULL, 0 };
    TEEC_UUID Uuid = {0xbc50d971,0xd4c9,0x42c4,{0x82,0xcb,0x34,0x3f,0xb7,0xf3,0x78,0x96}};
    TestSmcHook = Respond; mResponseResult = TEEC_ERROR_ITEM_NOT_FOUND;
    assert (TEEC_OpenSession (&Ctx, &Session, &Uuid, TEEC_LOGIN_PUBLIC, NULL, NULL, NULL) == TEEC_ERROR_ITEM_NOT_FOUND);
    TestExpectDiagnostic ("secure result", TEEC_ERROR_ITEM_NOT_FOUND, TEEC_ERROR_ITEM_NOT_FOUND);
    assert (Session.ctx == NULL && Session.session_id == 0);
    mResponseResult = TEEC_SUCCESS;
    mMutation = 7;
    assert (TEEC_OpenSession (&Ctx, &Session, &Uuid, TEEC_LOGIN_PUBLIC, NULL, NULL, NULL) == TEEC_ERROR_COMMUNICATION);
    TestExpectDiagnostic ("open response framing", 17, TEEC_ERROR_COMMUNICATION);
    mMutation = 0;
    assert (TEEC_OpenSession (&Ctx, &Session, &Uuid, TEEC_LOGIN_PUBLIC, NULL, NULL, NULL) == TEEC_SUCCESS);
    assert (Session.ctx == &Ctx && Session.session_id == 19);
    assert (OpteeClientCloseSession (&Session) == TEEC_SUCCESS);
  } else if (strcmp (Argv[1], "open_rejected") == 0) {
    TEEC_Context Ctx = { 1 }; TEEC_Session Session = { NULL, 0 };
    TEEC_UUID Uuid = {0xbc50d971,0xd4c9,0x42c4,{0x82,0xcb,0x34,0x3f,0xb7,0xf3,0x78,0x96}};
    TestSmcHook = Respond; mResponseResult = TEEC_SUCCESS; mMutation = 6;
    assert (TEEC_OpenSession (&Ctx, &Session, &Uuid, TEEC_LOGIN_PUBLIC, NULL, NULL, NULL) == TEEC_ERROR_COMMUNICATION);
    TestExpectDiagnostic ("response attribute", OPTEE_MSG_ATTR_TYPE_VALUE_OUTPUT, TEEC_ERROR_COMMUNICATION);
    assert (Session.ctx == NULL && Session.session_id == 0);
    assert (mSmcCalls == 2);
    mMutation = 9;
    assert (TEEC_OpenSession (&Ctx, &Session, &Uuid, TEEC_LOGIN_PUBLIC, NULL, NULL, NULL) == TEEC_ERROR_COMMUNICATION);
    TestExpectDiagnostic ("rejected open cleanup", 20, TEEC_ERROR_COMMUNICATION);
    assert (Session.ctx == NULL && Session.session_id == 0);
    assert (mSmcCalls == 4);
  } else if (strcmp (Argv[1], "partial") == 0) {
    TEEC_Context Ctx = { 1 }; TEEC_Session Session = { &Ctx, 7 }; TEEC_Operation Op = { 0 };
    UINT8 Input[16]; memset (Input, 0x5C, sizeof (Input));
    Op.paramTypes = TEEC_PARAM_TYPES (TEEC_MEMREF_TEMP_INPUT, TEEC_MEMREF_WHOLE, 0, 0);
    Op.params[0].tmpref.buffer = Input; Op.params[0].tmpref.size = sizeof (Input);
    assert (TEEC_InvokeCommand (&Session, 42, &Op, NULL) == TEEC_ERROR_NOT_SUPPORTED);
    for (UINTN I = 0; I < 8192; I++) { assert (TestPool[I] == 0); }
    assert (OpteeShmAlloc (TEST_POOL_SIZE, 4096) != NULL);
  } else if (strcmp (Argv[1], "rpc") == 0) {
    OPTEE_MSG_ARG *Arg = OpteeShmAlloc (OPTEE_MSG_ARG_SIZE (0), 4096);
    assert (Arg != NULL); TestSmcHook = RpcConversation;
    assert (OpteeCallWithArg (Arg) == TEEC_SUCCESS && mRpcStep == 4);
    assert (TestDelayCalls == 1 && TestDelayMicroseconds == 1000);
    OpteeShmFree (Arg);
  } else if (strcmp (Argv[1], "loop") == 0) {
    OPTEE_MSG_ARG *Arg = OpteeShmAlloc (OPTEE_MSG_ARG_SIZE (0), 4096);
    TestSmcHook = EndlessRpc;
    assert (OpteeCallWithArg (Arg) == TEEC_ERROR_COMMUNICATION);
    TestExpectDiagnostic ("RPC loop bound", OPTEE_RPC_MAX_CALLS, TEEC_ERROR_COMMUNICATION);
    assert (mSmcCalls == OPTEE_RPC_MAX_CALLS + 1);
    assert (!OpteeShmContains ((UINTN)TestPool + 4096, 1));
    assert (OpteeCallWithArg (Arg) == TEEC_ERROR_COMMUNICATION);
    assert (mSmcCalls == OPTEE_RPC_MAX_CALLS + 1);
    OpteeShmFree (Arg);
  } else if (strcmp (Argv[1], "transport") == 0) {
    TEEC_Context Ctx = { 1 }; TEEC_Session Session = { &Ctx, 7 };
    TestSmcHook = TransportFailure;
    assert (OpteeClientCloseSession (&Session) == TEEC_ERROR_BUSY);
    TestExpectDiagnostic ("SMC return", OPTEE_SMC_RETURN_ETHREAD_LIMIT, TEEC_ERROR_BUSY);
    assert (Session.ctx == &Ctx && Session.session_id == 7);
    assert (mSmcCalls == 1);
    assert (OpteeClientCloseSession (&Session) != TEEC_SUCCESS && mSmcCalls == 1);
  } else if (strcmp (Argv[1], "cache") == 0 || strcmp (Argv[1], "cache_failure") == 0 || strcmp (Argv[1], "cache_cookie") == 0) {
    mCacheBadCookie = strcmp (Argv[1], "cache_cookie") == 0;
    mCacheBad = strcmp (Argv[1], "cache_failure") == 0;
    mRpcArgument = OpteeShmAllocOwned (64, 4096, OPTEE_SHM_RPC_ARG);
    assert (mRpcArgument != NULL); memset (mRpcArgument, 0x5C, 4096);
    TestCacheHook = CachePeer;
    assert (OpteeClientApiFinalize () == !(mCacheBad || mCacheBadCookie));
    if (mCacheBad || mCacheBadCookie) {
      TestExpectDiagnostic ("SHM cache drain", mCacheBad ? 2 : (UINTN)mRpcArgument + 1, TEEC_ERROR_COMMUNICATION);
    }
    assert (mCacheCalls == ((mCacheBad || mCacheBadCookie) ? 1 : 2));
    assert (HostLivePoolAllocations () == 0);
    for (UINTN I = 0; I < 4096; I++) { assert (((UINT8 *)mRpcArgument)[I] == 0); }
    if (mCacheBad || mCacheBadCookie) { assert (OpteeClientApiInitialize (NULL) == EFI_DEVICE_ERROR); }
    return 0;
  } else if (strcmp (Argv[1], "finalize") == 0) {
    UINT8 *P = OpteeShmAlloc (17, 4096); memset (P, 0x5C, 4096);
    assert (OpteeClientApiFinalize ());
    for (UINTN I = 0; I < 4096; I++) { assert (P[I] == 0); }
    assert (HostLivePoolAllocations () == 0); return 0;
  } else { abort (); }
  TestFini ();
  return 0;
}
