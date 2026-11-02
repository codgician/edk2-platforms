/** @file Nonsecret, single-line secure-world rejection diagnostics. */
#include "TestSupport.h"
#include <Library/UefiBootServicesTableLib.h>
#include "../OpteeClientDiagnostics.h"

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
  TestInit ();
  Arg = OpteeShmAllocOwned (OPTEE_MSG_ARG_SIZE (4), 4096, OPTEE_SHM_RPC_ARG);
  Payload = OpteeShmAllocOwned (64, 4096, OPTEE_SHM_RPC_APPL);
  assert (Arg != NULL && Payload != NULL);
  // A path-carrying rejection names the check, never the path.
  strcpy (Payload, "SECRET-CANARY");
  Arg->Cmd = OPTEE_RPC_CMD_FS;
  Arg->NumParams = 2;
  Arg->Params[0].Attr = OPTEE_MSG_ATTR_TYPE_VALUE_INPUT;
  Arg->Params[0].U.Value.A = OPTEE_RPC_FS_REMOVE;
  Arg->Params[1].Attr = OPTEE_MSG_ATTR_TYPE_TMEM_INPUT;
  Arg->Params[1].U.Tmem.BufPtr = (UINTN)Payload;
  Arg->Params[1].U.Tmem.ShmRef = (UINTN)Payload;
  Arg->Params[1].U.Tmem.Size = 64;
  assert (Dispatch (Arg));
  TestExpectDiagnostic ("REE-FS file missing", 0, TEEC_ERROR_ITEM_NOT_FOUND);
  Arg->NumParams = 1;
  assert (Dispatch (Arg));
  TestExpectDiagnostic ("REE-FS remove framing", 1, TEEC_ERROR_BAD_PARAMETERS);
  Arg->Params[0].U.Value.A = 0x1234;
  assert (Dispatch (Arg));
  TestExpectDiagnostic ("REE-FS command", 0x1234, TEEC_ERROR_NOT_SUPPORTED);
  Smc.Arg0 = OPTEE_SMC_RETURN_RPC_PREFIX | 0x1234;
  {
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL Console = { .OutputString = ConsoleOutput };
    EFI_SYSTEM_TABLE Table = { .ConOut = &Console };
    gST = &Table;
    assert (!OpteeRpcHandle (&Smc));
    TestExpectDiagnostic ("RPC function", 0x1234, TEEC_ERROR_NOT_SUPPORTED);
    assert (mConsoleCalls == 1);
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
