/** @file Nonsecret, single-line secure-world rejection diagnostics. */
#include "TestSupport.h"
#include <Library/UefiBootServicesTableLib.h>
#include "../OpteeClientDiagnostics.h"

// Original-source replay reaches the earlier missing-diagnostic assertion.
extern VOID OpteeClientApiExitBootServices (VOID) __attribute__((weak));
STATIC UINTN mConsoleCalls;
STATIC EFI_STATUS EFIAPI ConsoleOutput (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *Line) {
  CHAR8 Copy[512];
  UINTN Index;
  (VOID)This;
  for (Index = 0; Line[Index] != 0; Index++) {
    assert (Index < sizeof (Copy) - 1);
    Copy[Index] = (CHAR8)Line[Index];
  }
  Copy[Index] = 0;
  assert (strcmp (Copy, TestDiagnostic) == 0);
  mConsoleCalls++;
  return EFI_SUCCESS;
}

STATIC BOOLEAN Dispatch (OPTEE_MSG_ARG *Arg) {
  ARM_SMC_ARGS Args = { 0 };
  Args.Arg0 = OPTEE_SMC_RETURN_RPC_PREFIX | OPTEE_SMC_RPC_FUNC_CMD;
  Args.Arg1 = (UINT64)(UINTN)Arg >> 32;
  Args.Arg2 = (UINT32)(UINTN)Arg;
  return OpteeRpcHandle (&Args);
}

int main (void) {
  OPTEE_MSG_ARG *Arg;
  ARM_SMC_ARGS Smc = { 0 };
  VOID *Payload;
  TestResetServices ();
  TestCapabilityFlags = 0;
  assert (OpteeClientApiInitialize (NULL) == EFI_UNSUPPORTED);
  TestExpectDiagnostic ("SHM capabilities", 0, EFI_UNSUPPORTED);
  TestResetServices ();
  TestShmFlags = 3;
  assert (OpteeClientApiInitialize (NULL) == EFI_UNSUPPORTED);
  TestExpectDiagnostic ("SHM cache mode", 3, EFI_UNSUPPORTED);
  TestInit ();
  Arg = OpteeShmAllocOwned (OPTEE_MSG_ARG_SIZE (4), 4096, OPTEE_SHM_RPC_ARG);
  assert (Arg != NULL);
  Arg->NumParams = 5;
  assert (!Dispatch (Arg));
  TestExpectDiagnostic ("RPC parameter count", 5, TEEC_ERROR_BAD_PARAMETERS);
  Arg->NumParams = 0;
  Arg->Cmd = 0x1234;
  assert (Dispatch (Arg));
  TestExpectDiagnostic ("RPC command", 0x1234, TEEC_ERROR_NOT_SUPPORTED);
  Arg->Cmd = OPTEE_RPC_CMD_SUSPEND;
  Arg->NumParams = 1;
  Arg->Params[0].Attr = OPTEE_MSG_ATTR_TYPE_VALUE_INPUT;
  Arg->Params[0].U.Value.A = 101;
  assert (Dispatch (Arg));
  TestExpectDiagnostic ("RPC suspend", 101, TEEC_ERROR_BAD_PARAMETERS);
  Arg->Cmd = OPTEE_RPC_CMD_LOAD_TA;
  Arg->NumParams = 2;
  ZeroMem (&Arg->Params[0].U.Value, sizeof (Arg->Params[0].U.Value));
  Arg->Params[1].Attr = OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT;
  Arg->Params[1].U.Tmem.BufPtr = 0;
  Arg->Params[1].U.Tmem.Size = 0;
  Arg->Params[1].U.Tmem.ShmRef = 0;
  assert (Dispatch (Arg));
  TestExpectDiagnostic ("TA admission", 0, TEEC_ERROR_ITEM_NOT_FOUND);
  Payload = OpteeShmAllocOwned (64, 4096, OPTEE_SHM_RPC_APPL);
  assert (Payload != NULL);
  strcpy (Payload, "SECRET-CANARY");
  Arg->Cmd = OPTEE_RPC_CMD_FS;
  Arg->NumParams = 3;
  Arg->Params[0].U.Value.A = OPTEE_RPC_FS_CREATE;
  Arg->Params[1].Attr = OPTEE_MSG_ATTR_TYPE_TMEM_INPUT;
  Arg->Params[1].U.Tmem.BufPtr = (UINTN)Payload;
  Arg->Params[1].U.Tmem.ShmRef = (UINTN)Payload;
  Arg->Params[1].U.Tmem.Size = 64;
  Arg->Params[2].Attr = OPTEE_MSG_ATTR_TYPE_VALUE_OUTPUT;
  assert (Dispatch (Arg));
  assert (Arg->Ret == TEEC_SUCCESS && TestDiagnosticCount == 0);
  Arg->Params[0].U.Value.B = Arg->Params[2].U.Value.A;
  Arg->Params[0].U.Value.A = OPTEE_RPC_FS_TRUNCATE;
  Arg->Params[0].U.Value.C = 1024 * 1024 + 1;
  Arg->NumParams = 1;
  assert (Dispatch (Arg));
  TestExpectDiagnostic ("REE-FS capacity", 1024 * 1024 + 1, TEEC_ERROR_OUT_OF_MEMORY);
  Arg->NumParams = 0;
  assert (Dispatch (Arg));
  TestExpectDiagnostic ("REE-FS framing", 0, TEEC_ERROR_BAD_PARAMETERS);
  Arg->NumParams = 1;
  Arg->Params[0].U.Value.A = OPTEE_RPC_FS_RENAME;
  assert (Dispatch (Arg));
  TestExpectDiagnostic ("REE-FS rename framing", 1, TEEC_ERROR_BAD_PARAMETERS);
  Arg->Params[0].U.Value.A = OPTEE_RPC_FS_REMOVE;
  assert (Dispatch (Arg));
  TestExpectDiagnostic ("REE-FS remove framing", 1, TEEC_ERROR_BAD_PARAMETERS);
  Arg->Params[0].U.Value.A = OPTEE_RPC_FS_OPENDIR;
  assert (Dispatch (Arg));
  TestExpectDiagnostic ("REE-FS directory framing", 1, TEEC_ERROR_BAD_PARAMETERS);
  Arg->NumParams = 3;
  assert (Dispatch (Arg));
  TestExpectDiagnostic ("REE-FS directory unsupported", OPTEE_RPC_FS_OPENDIR, TEEC_ERROR_ITEM_NOT_FOUND);
  Arg->Params[0].U.Value.A = 0x1234;
  assert (Dispatch (Arg));
  TestExpectDiagnostic ("REE-FS command", 0x1234, TEEC_ERROR_NOT_SUPPORTED);
  Arg->Cmd = OPTEE_RPC_CMD_LOAD_TA;
  Arg->NumParams = 2;
  Arg->Params[1].Attr = OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT;
  Arg->Params[1].U.Tmem.Size = 65;
  assert (Dispatch (Arg));
  TestExpectDiagnostic ("TA framing", 65, TEEC_ERROR_BAD_PARAMETERS);
  Arg->Params[1].U.Tmem.Size = 1;
  Arg->Params[1].U.Tmem.BufPtr = (UINTN)Payload + 65;
  assert (Dispatch (Arg));
  TestExpectDiagnostic ("TA framing", (UINTN)Payload + 65, TEEC_ERROR_BAD_PARAMETERS);
  Arg->Params[1].U.Tmem.BufPtr = 0;
  assert (Dispatch (Arg));
  TestExpectDiagnostic ("TA framing", 1, TEEC_ERROR_BAD_PARAMETERS);
  Smc.Arg0 = OPTEE_SMC_RETURN_RPC_PREFIX | 0x1234;
  assert (!OpteeRpcHandle (&Smc));
  TestExpectDiagnostic ("RPC function", 0x1234, TEEC_ERROR_NOT_SUPPORTED);
  {
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL Console = { .OutputString = ConsoleOutput };
    EFI_SYSTEM_TABLE Table = { .ConOut = &Console };
    gST = &Table;
    assert (!OpteeRpcHandle (&Smc));
    TestExpectDiagnostic ("RPC function", 0x1234, TEEC_ERROR_NOT_SUPPORTED);
    assert (mConsoleCalls == 1);
    assert (OpteeClientApiExitBootServices != NULL);
    OpteeClientApiExitBootServices ();
    TestFailAllocation (1);
    assert (!OpteeRpcHandle (&Smc));
    TestExpectDiagnostic ("RPC function", 0x1234, TEEC_ERROR_NOT_SUPPORTED);
    assert (mConsoleCalls == 1);
    // The failed-allocation token remains pending: logging called no allocator.
    assert (AllocatePool (1) == NULL);
    OpteeClientApiExitBootServices ();
    gST = (EFI_SYSTEM_TABLE *)(UINTN)1;
    assert (!OpteeRpcHandle (&Smc));
    TestExpectDiagnostic ("RPC function", 0x1234, TEEC_ERROR_NOT_SUPPORTED);
    assert (mConsoleCalls == 1);
    TestFailAllocation (0);
    gST = NULL;
  }
  TestFini ();
  return 0;
}
