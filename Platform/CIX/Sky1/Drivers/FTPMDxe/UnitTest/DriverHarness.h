/** @file
  Scripted external services for the real FTPMDxe translation unit.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef FTPM_DRIVER_HARNESS_H_
#define FTPM_DRIVER_HARNESS_H_

#include <Uefi.h>
#include <PiDxe.h>
#include <Guid/EventGroup.h>
#include <Library/tee_client_api.h>
#include <Protocol/FTPMInitProtocol.h>

#define TEST_TPM_MAX  0x1000

extern FTPM_INIT_PROTOCOL  *TestProtocol;
extern UINT8               TestResponse[TEST_TPM_MAX];
extern UINTN               TestResponseBytes;
extern UINTN               TestReturnedSize;
extern TEEC_Result         TestInvokeResult;
extern TEEC_Result         TestCloseResult;
extern TEEC_Result         TestOpenResult;
extern EFI_STATUS          TestEventResult;
extern EFI_STATUS          TestInstallResult;
extern BOOLEAN             TestFinalizeResult;
extern BOOLEAN             TestCloseSawReadinessRevoked;
extern UINT32              TestLastDiagnosticCommands;
extern UINT32              TestLastDiagnosticFailures;
extern UINTN               TestInvocations;
extern UINTN               TestCheckedCloses;
extern UINTN               TestLegacyCloses;
extern UINTN               TestFinalizes;
extern UINTN               TestVariableWrites;
extern BOOLEAN             TestAtEbs;
extern BOOLEAN             TestHalted;
extern BOOLEAN             TestCheckZeroResponse;
extern BOOLEAN             TestCheckPrivateCommand;
extern UINT8               *TestCallerInput;
extern UINT8               *TestCallerOutput;
extern UINT8               *TestSavedCommand;
extern UINT8               *TestSavedResponse;
extern UINTN               TestSavedCommandSize;
extern UINTN               TestSavedResponseSize;
extern CHAR8               TestSerial[4096];
extern UINTN               TestConsoleWrites;
extern EFI_STATUS          TestClientResult;
extern TEEC_Result         TestContextResult;
extern EFI_STATUS          TestReadyEventResult;
extern EFI_STATUS          TestLoadResult;
extern EFI_STATUS          TestStartResult;
extern UINTN               TestResidentImages;
extern UINTN               TestClosedEvents;
extern UINTN               TestContextFinalizes;
extern UINTN               TestCapExtends;
extern EFI_STATUS          TestCapResult;
extern TEEC_Result         TestCapDiscoveryResult;
extern BOOLEAN             TestCapMalformedFrame;
extern BOOLEAN             TestCapMalformedExtend;
extern BOOLEAN             TestCapMoreData;
extern UINTN               TestCapFailPcr;

BOOLEAN
TestPrivateReady (
  VOID
  );

VOID
TestCapStoppedEntry (
  VOID
  );

VOID
TestWithoutConsole (
  VOID
  );

BOOLEAN
TestCallbacksResident (
  VOID
  );

BOOLEAN
TestHandshakeCleared (
  VOID
  );

EFI_STATUS
TestUnloadResident (
  VOID
  );

VOID
TestReset (
  VOID
  );

EFI_STATUS
TestStart (
  VOID
  );

VOID
TestExitBootServices (
  VOID
  );

VOID
TestReadyToBoot (
  VOID
  );

BOOLEAN
TestSessionStillOpen (
  VOID
  );

VOID
TestHeader (
  UINT8   *Buffer,
  UINT16  Tag,
  UINT32  Size
  );

VOID
TestRequire (
  BOOLEAN  Condition
  );

BOOLEAN
TestAllZero (
  CONST UINT8  *Buffer,
  UINTN        Size
  );

#endif
