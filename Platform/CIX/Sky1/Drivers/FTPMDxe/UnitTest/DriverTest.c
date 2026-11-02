/** @file
  Security regressions at the real fTPM protocol boundary.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "DriverHarness.h"
#include <IndustryStandard/Tpm20.h>
#include <Library/Tpm2CommandLib.h>

STATIC UINT8  mCommand[TEST_TPM_MAX + 1];
STATIC UINT8  mOutput[TEST_TPM_MAX + 2];

/** Diagnostics carry fixed text plus a status; count the emitted lines. **/
STATIC UINTN
SerialLines (
  VOID
  )
{
  UINTN        Lines = 0;
  CONST CHAR8  *Text = TestSerial;

  while ((Text = strstr (Text, "\r\n")) != NULL) {
    Lines++;
    Text += 2;
  }

  return Lines;
}

STATIC int
Before (
  void  **State
  )
{
  TestReset ();
  assert_int_equal (TestStart (), EFI_SUCCESS);
  assert_non_null (TestProtocol);
  memset (mCommand, 0x71, sizeof (mCommand));
  TestHeader (mCommand, TPM_ST_NO_SESSIONS, 10);
  memset (mOutput, 0xA5, sizeof (mOutput));
  TestCallerOutput = mOutput + 1;
  return 0;
}

STATIC EFI_STATUS
Submit (
  UINT32  *Size
  )
{
  return TestProtocol->SubmitCommand (TestProtocol, 10, mCommand, Size, mOutput + 1);
}

STATIC void
ResponseLengths (
  void  **State
  )
{
  CONST UINTN  Lengths[] = { 0, 1, 9, 10, 32, 33, MAX_UINT32, (UINTN)MAX_UINT32 + 1 };

  for (UINTN Index = 0; Index < sizeof (Lengths) / sizeof (Lengths[0]); Index++) {
    UINT32  Capacity = 32;
    UINTN   Length   = Lengths[Index];
    TestReturnedSize  = Length;
    TestResponseBytes = 32;
    TestHeader (TestResponse, TPM_ST_NO_SESSIONS, (UINT32)Length);
    memset (mOutput, 0xA5, sizeof (mOutput));
    EFI_STATUS  Status = Submit (&Capacity);
    if ((Length == 10) || (Length == 32)) {
      assert_int_equal (Status, EFI_SUCCESS);
      assert_int_equal (Capacity, Length);
      assert_memory_equal (mOutput + 1, TestResponse, Length);
    } else {
      assert_int_equal (Status, EFI_DEVICE_ERROR);
      assert_int_equal (Capacity, 0);
      for (UINTN Byte = 0; Byte < sizeof (mOutput); Byte++) {
        assert_int_equal (mOutput[Byte], 0xA5);
      }
    }

    assert_int_equal (mOutput[0], 0xA5);
    assert_int_equal (mOutput[33], 0xA5);
  }
}

STATIC void
ResponseHeaders (
  void  **State
  )
{
  CONST UINT16  Tags[] = { TPM_ST_NO_SESSIONS, TPM_ST_SESSIONS, 0, 0x8003, 0xFFFF };

  for (UINTN Index = 0; Index < sizeof (Tags) / sizeof (Tags[0]); Index++) {
    UINT32  Capacity = 32;
    TestHeader (TestResponse, Tags[Index], 10);
    assert_int_equal (Submit (&Capacity), Index < 2 ? EFI_SUCCESS : EFI_DEVICE_ERROR);
  }

  for (UINT32 Size = 0; Size <= 11; Size++) {
    UINT32  Capacity = 32;
    TestHeader (TestResponse, TPM_ST_NO_SESSIONS, Size);
    assert_int_equal (Submit (&Capacity), Size == 10 ? EFI_SUCCESS : EFI_DEVICE_ERROR);
  }
}

STATIC void
ResponseMaximumAndEmptyWrite (
  void  **State
  )
{
  UINT32  Capacity = MAX_UINT32;

  TestResponseBytes = TEST_TPM_MAX;
  TestReturnedSize  = TEST_TPM_MAX;
  memset (TestResponse, 0x3C, sizeof (TestResponse));
  TestHeader (TestResponse, TPM_ST_SESSIONS, TEST_TPM_MAX);
  assert_int_equal (Submit (&Capacity), EFI_SUCCESS);
  assert_int_equal (Capacity, TEST_TPM_MAX);
  assert_memory_equal (mOutput + 1, TestResponse, TEST_TPM_MAX);
  assert_int_equal (mOutput[TEST_TPM_MAX + 1], 0xA5);
  TestResponseBytes = 0;
  TestReturnedSize  = 10;
  Capacity          = 32;
  memset (mOutput, 0xA5, sizeof (mOutput));
  assert_int_equal (Submit (&Capacity), EFI_DEVICE_ERROR);
  assert_int_equal (Capacity, 0);
  assert_int_equal (mOutput[1], 0xA5);
}

STATIC void
CommandBoundaries (
  void  **State
  )
{
  CONST UINT32  Sizes[] = { 0, 1, 9, 10, TEST_TPM_MAX, TEST_TPM_MAX + 1, MAX_UINT32 };

  for (UINTN Index = 0; Index < sizeof (Sizes) / sizeof (Sizes[0]); Index++) {
    UINT32  Capacity = 32;
    UINT32  Size     = Sizes[Index];
    UINTN   Calls    = TestInvocations;
    TestHeader (mCommand, TPM_ST_NO_SESSIONS, Size);
    EFI_STATUS  Status = TestProtocol->SubmitCommand (TestProtocol, Size, mCommand, &Capacity, mOutput + 1);
    BOOLEAN     Valid  = Size >= 10 && Size <= TEST_TPM_MAX;
    assert_int_equal (Status, Valid ? EFI_SUCCESS : EFI_INVALID_PARAMETER);
    assert_int_equal (TestInvocations, Calls + (Valid ? 1 : 0));
  }

  UINT32  Capacity = 32;
  TestHeader (mCommand, TPM_ST_NO_SESSIONS, 11);
  UINTN  Calls = TestInvocations;
  assert_int_equal (Submit (&Capacity), EFI_INVALID_PARAMETER);
  assert_int_equal (TestInvocations, Calls);
}

STATIC void
PointerContracts (
  void  **State
  )
{
  UINT32  Size  = 32;
  UINTN   Calls = TestInvocations;

  assert_int_equal (TestProtocol->SubmitCommand (TestProtocol, 10, NULL, &Size, mOutput), EFI_INVALID_PARAMETER);
  assert_int_equal (TestProtocol->SubmitCommand (TestProtocol, 10, mCommand, NULL, mOutput), EFI_INVALID_PARAMETER);
  assert_int_equal (TestProtocol->SubmitCommand (TestProtocol, 10, mCommand, &Size, NULL), EFI_INVALID_PARAMETER);
  Size             = 0;
  TestInvokeResult = TEEC_ERROR_SHORT_BUFFER;
  TestReturnedSize = 10;
  assert_int_equal (TestProtocol->SubmitCommand (TestProtocol, 10, mCommand, &Size, NULL), EFI_BUFFER_TOO_SMALL);
  assert_int_equal (Size, 10);
  assert_int_equal (TestInvocations, Calls + 1);
}

STATIC void
ShortBuffer (
  void  **State
  )
{
  CONST UINTN  Sizes[] = { 0, 9, 10, 33, TEST_TPM_MAX, TEST_TPM_MAX + 1, MAX_UINT32, (UINTN)MAX_UINT32 + 1 };

  TestInvokeResult = TEEC_ERROR_SHORT_BUFFER;
  for (UINTN Index = 0; Index < sizeof (Sizes) / sizeof (Sizes[0]); Index++) {
    UINT32  Capacity = 32;
    TestReturnedSize = Sizes[Index];
    BOOLEAN  Valid = Sizes[Index] > Capacity && Sizes[Index] <= TEST_TPM_MAX;
    assert_int_equal (Submit (&Capacity), Valid ? EFI_BUFFER_TOO_SMALL : EFI_DEVICE_ERROR);
    assert_int_equal (Capacity, Valid ? Sizes[Index] : 0);
    for (UINTN Byte = 0; Byte < sizeof (mOutput); Byte++) {
      assert_int_equal (mOutput[Byte], 0xA5);
    }
  }
}

STATIC void
TaErrors (
  void  **State
  )
{
  CONST TEEC_Result  Results[] = {
    TEEC_ERROR_GENERIC,     TEEC_ERROR_OUT_OF_MEMORY,
    TEEC_ERROR_TARGET_DEAD, TEEC_ERROR_COMMUNICATION, TEEC_ERROR_BAD_STATE
  };

  for (UINTN Index = 0; Index < sizeof (Results) / sizeof (Results[0]); Index++) {
    Before (State);
    UINT32  Size = 32;
    TestInvokeResult = Results[Index];
    assert_int_equal (Submit (&Size), EFI_DEVICE_ERROR);
    assert_int_equal (Size, 0);
    for (UINTN Byte = 0; Byte < sizeof (mOutput); Byte++) {
      assert_int_equal (mOutput[Byte], 0xA5);
    }

    assert_int_equal (TestProtocol->GetFTPMStatus (TestProtocol), Index < 2);
  }
}

STATIC void
SecretIsolation (
  void  **State
  )
{
  TestCheckZeroResponse   = TRUE;
  UINT32  Size = 32;
  assert_int_equal (Submit (&Size), EFI_SUCCESS);
  assert_true (TestAllZero (TestSavedResponse, TEST_TPM_MAX));
  assert_int_equal (mCommand[9], 0);
  assert_int_equal (mCommand[10], 0x71);
  TestInvokeResult = TEEC_ERROR_GENERIC;
  Size             = 32;
  assert_int_equal (Submit (&Size), EFI_DEVICE_ERROR);
  assert_true (TestAllZero (TestSavedResponse, TEST_TPM_MAX));
  TestInvokeResult = TEEC_ERROR_SHORT_BUFFER;
  TestReturnedSize = 64;
  Size             = 32;
  assert_int_equal (Submit (&Size), EFI_BUFFER_TOO_SMALL);
  assert_true (TestAllZero (TestSavedResponse, TEST_TPM_MAX));
}

STATIC void
CloseSuccessTwice (
  void  **State
  )
{
  assert_true (TestProtocol->GetFTPMStatus (TestProtocol));
  UINTN  Writes = TestVariableWrites;
  TestExitBootServices ();
  assert_int_equal (TestCheckedCloses, 1);
  assert_true (TestCloseSawReadinessRevoked);
  assert_false (TestSessionStillOpen ());
  assert_false (TestProtocol->GetFTPMStatus (TestProtocol));
  assert_int_equal (TestProtocol->FTPMOpenSession (TestProtocol), EFI_UNSUPPORTED);
  UINT32  Size = 32;
  assert_int_equal (Submit (&Size), EFI_NOT_READY);
  TestExitBootServices ();
  assert_int_equal (TestCheckedCloses, 1);
  assert_int_equal (TestVariableWrites, Writes);
}

STATIC void
CloseFailureReturns (
  void  **State
  )
{
  CONST TEEC_Result  Failures[] = {
    TEEC_ERROR_GENERIC, TEEC_ERROR_OUT_OF_MEMORY,
    TEEC_ERROR_COMMUNICATION, TEEC_ERROR_BUSY
  };
  UINTN   Index;
  UINTN   Writes;
  UINT32  Size;

  for (Index = 0; Index < sizeof (Failures) / sizeof (Failures[0]); Index++) {
    Before (State);
    TestCloseResult = Failures[Index];
    Writes          = TestVariableWrites;
    TestExitBootServices ();
    assert_false (TestHalted);
    assert_int_equal (TestCheckedCloses, 1);
    assert_false (TestProtocol->GetFTPMStatus (TestProtocol));
    assert_true (TestCloseSawReadinessRevoked);
    assert_true (TestSessionStillOpen ());
    Size = 32;
    assert_int_equal (Submit (&Size), EFI_NOT_READY);
    TestExitBootServices ();
    assert_int_equal (TestCheckedCloses, 2);
    assert_true (TestSessionStillOpen ());
    assert_int_equal (SerialLines (), 2);
    assert_int_equal (TestConsoleWrites, 0);
    assert_int_equal (TestVariableWrites, Writes);
    TestCloseResult = TEEC_SUCCESS;
    TestExitBootServices ();
    assert_int_equal (TestCheckedCloses, 3);
    assert_false (TestSessionStillOpen ());
    assert_false (TestProtocol->GetFTPMStatus (TestProtocol));
    TestExitBootServices ();
    assert_int_equal (TestCheckedCloses, 3);
  }
}

STATIC void
InstallFailureCleanup (
  void  **State
  )
{
  TestReset ();
  TestInstallResult = EFI_OUT_OF_RESOURCES;
  assert_int_equal (TestStart (), EFI_SUCCESS);
  assert_null (TestProtocol);
  assert_int_equal (TestCheckedCloses, 0);
  assert_true (TestSessionStillOpen ());
  assert_int_equal (TestCapExtends, 8);
  assert_int_equal (TestInvocations, 9);
  assert_false (TestPrivateReady ());
  assert_true (TestAllZero (TestSavedResponse, TEST_TPM_MAX));
  assert_int_equal (SerialLines (), 3);
}

STATIC void
InstallAndCloseFailureRetainsResident (
  void  **State
  )
{
  TestReset ();
  TestInstallResult = EFI_OUT_OF_RESOURCES;
  TestCloseResult = TEEC_ERROR_COMMUNICATION;
  assert_int_equal (TestStart (), EFI_SUCCESS);
  assert_false (TestHalted);
  assert_null (TestProtocol);
  assert_true (TestSessionStillOpen ());
  assert_int_equal (TestCheckedCloses, 0);
  assert_int_equal (SerialLines (), 3);
  assert_int_equal (TestConsoleWrites, 3);
  TestExitBootServices ();
  assert_true (TestCloseSawReadinessRevoked);
  assert_true (TestSessionStillOpen ());
  assert_int_equal (TestCheckedCloses, 1);
  assert_int_equal (SerialLines (), 4);
  TestAtEbs = FALSE;
  TestCloseResult = TEEC_SUCCESS;
  TestExitBootServices ();
  assert_false (TestSessionStillOpen ());
}

STATIC void
InstallFailureCapStops (
  void  **State
  )
{
  // Case 0: discovery fails; 1: malformed discovery frame; 2-9: PCR n-2 fails.
  for (UINTN Case = 0; Case < 10; Case++) {
    TestReset ();
    TestInstallResult      = EFI_OUT_OF_RESOURCES;
    TestCapDiscoveryResult = (Case == 0) ? TEEC_ERROR_COMMUNICATION : TEEC_SUCCESS;
    TestCapMalformedFrame  = (Case == 1);
    TestCapResult          = (Case >= 2) ? EFI_DEVICE_ERROR : EFI_SUCCESS;
    TestCapFailPcr         = (Case >= 2) ? Case - 2 : 0;
    TestCapStoppedEntry ();
    assert_true (TestHalted);
    assert_int_equal (TestCapExtends, (Case >= 2) ? Case - 1 : 0);
    assert_int_equal (SerialLines (), 2);
  }
}

STATIC void
InstallFailureAbsentTaSkipsCap (
  void  **State
  )
{
  TestReset ();
  TestOpenResult = TEEC_ERROR_ITEM_NOT_FOUND;
  TestInstallResult = EFI_OUT_OF_RESOURCES;
  assert_int_equal (TestStart (), EFI_SUCCESS);
  assert_false (TestHalted);
  assert_null (TestProtocol);
  assert_false (TestSessionStillOpen ());
  assert_false (TestPrivateReady ());
  assert_int_equal (TestInvocations, 0);
  assert_int_equal (TestCapExtends, 0);
  assert_int_equal (SerialLines (), 2);
  TestExitBootServices ();
  assert_int_equal (TestCheckedCloses, 0);
}

STATIC void
OpenFailure (
  void  **State
  )
{
  TestReset ();
  TestOpenResult = TEEC_ERROR_GENERIC;
  assert_int_equal (TestStart (), EFI_SUCCESS);
  assert_false (TestProtocol->GetFTPMStatus (TestProtocol));
  assert_int_equal (SerialLines (), 1);
  TestExitBootServices ();
  assert_int_equal (TestCheckedCloses, 0);
}

STATIC void
EventRegistrationFailure (
  void  **State
  )
{
  TestReset ();
  TestEventResult = EFI_OUT_OF_RESOURCES;
  assert_int_equal (TestStart (), EFI_OUT_OF_RESOURCES);
  assert_null (TestProtocol);
  assert_int_equal (SerialLines (), 1);
}

STATIC void
LateEventRegistrationFailure (
  void  **State
  )
{
  // A failed second registration must not leave the first event pointing
  // into an image that is about to be unloaded.
  TestReset ();
  TestEventResult   = EFI_OUT_OF_RESOURCES;
  TestEventFailCall = 2;
  assert_int_equal (TestStart (), EFI_OUT_OF_RESOURCES);
  assert_null (TestProtocol);
  assert_int_equal (TestClosedEvents, 1);
  assert_int_equal (TestInvocations, 0);
  assert_int_equal (SerialLines (), 1);
}

STATIC void
InitializationDiagnostics (
  void  **State
  )
{
  TestReset ();
  TestClientResult = EFI_DEVICE_ERROR;
  assert_int_equal (TestStart (), EFI_SUCCESS);
  assert_false (TestProtocol->GetFTPMStatus (TestProtocol));
  assert_int_equal (SerialLines (), 1);
  assert_int_equal (TestConsoleWrites, 1);
  TestReset ();
  TestReadyEventResult = EFI_OUT_OF_RESOURCES;
  assert_int_equal (TestStart (), EFI_SUCCESS);
  assert_true (TestProtocol->GetFTPMStatus (TestProtocol));
  assert_int_equal (SerialLines (), 1);
}

STATIC void
FinalMeasurementBeforeClose (
  void  **State
  )
{
  // DXE Core signals BeforeExitBootServices before accepting the map key;
  // Tcg2Dxe's TPL_NOTIFY final measurements then run on serial-only output.
  UINT32  Size = 32;
  TestBeforeExitBootServices ();
  assert_true (TestSerialOnly);
  TestAtEbs               = TRUE;
  TestCheckZeroResponse   = TRUE;
  assert_int_equal (Submit (&Size), EFI_SUCCESS);
  assert_int_equal (TestInvocations, 1);
  assert_int_equal (TestCheckedCloses, 0);
  assert_int_equal (TestVariableWrites, 1);
  TestExitBootServices ();
  assert_int_equal (TestCheckedCloses, 1);
  assert_int_equal (TestVariableWrites, 1);
}


STATIC void
DiagnosticsBeforeEbs (
  void  **State
  )
{
  UINT32  Size = 32;

  TestInvokeResult = TEEC_ERROR_GENERIC;
  assert_int_equal (Submit (&Size), EFI_DEVICE_ERROR);
  TestReadyToBoot ();
  assert_int_equal (TestVariableWrites, 2);
  assert_int_equal (TestLastDiagnosticCommands, 1);
  assert_int_equal (TestLastDiagnosticFailures, 1);
  TestExitBootServices ();
  assert_int_equal (TestVariableWrites, 2);
}


STATIC void
CapabilityConsumer (
  void  **State
  )
{
  TPMS_CAPABILITY_DATA  Capability;
  TPMI_YES_NO           More = 0xA5;

  memset (&Capability, 0xA5, sizeof (Capability));
  TestReturnedSize = MAX_UINT32;
  TestHeader (TestResponse, TPM_ST_NO_SESSIONS, MAX_UINT32);
  assert_int_equal (Tpm2GetCapability (TPM_CAP_TPM_PROPERTIES, TPM_PT_FAMILY_INDICATOR, 1, &More, &Capability), EFI_DEVICE_ERROR);
  assert_int_equal (More, 0xA5);
  assert_int_equal (((UINT8 *)&Capability)[0], 0xA5);
  TestReturnedSize  = 19;
  TestResponseBytes = 19;
  TestHeader (TestResponse, TPM_ST_NO_SESSIONS, 19);
  TestResponse[10] = 0;
  CONST UINT8  Data[8] = { 0, 0, 0, 6, 0, 0, 0, 0 };
  memcpy (TestResponse + 11, Data, sizeof (Data));
  assert_int_equal (Tpm2GetCapability (TPM_CAP_TPM_PROPERTIES, TPM_PT_FAMILY_INDICATOR, 1, &More, &Capability), EFI_SUCCESS);
  assert_int_equal (More, 0);
  assert_memory_equal (&Capability, Data, sizeof (Data));
}

int
main (
  void
  )
{
  if (getenv ("FTPM_TEST_FILTER") != NULL) {
    cmocka_set_test_filter (getenv ("FTPM_TEST_FILTER"));
  }

  const struct CMUnitTest  Tests[] = {
    cmocka_unit_test_setup (ResponseLengths,              Before),
    cmocka_unit_test_setup (ResponseHeaders,              Before),
    cmocka_unit_test_setup (ResponseMaximumAndEmptyWrite, Before),
    cmocka_unit_test_setup (CommandBoundaries,            Before),
    cmocka_unit_test_setup (PointerContracts,             Before),
    cmocka_unit_test_setup (ShortBuffer,                  Before),
    cmocka_unit_test_setup (TaErrors,                     Before),
    cmocka_unit_test_setup (SecretIsolation,              Before),
    cmocka_unit_test_setup (CloseSuccessTwice,            Before),
    cmocka_unit_test_setup (CloseFailureReturns,          Before),
    cmocka_unit_test_setup (InstallFailureCleanup,        Before),
    cmocka_unit_test_setup (InstallAndCloseFailureRetainsResident, Before),
    cmocka_unit_test_setup (InstallFailureCapStops,       Before),
    cmocka_unit_test_setup (InstallFailureAbsentTaSkipsCap, Before),
    cmocka_unit_test_setup (OpenFailure,                  Before),
    cmocka_unit_test_setup (EventRegistrationFailure,     Before),
    cmocka_unit_test_setup (LateEventRegistrationFailure, Before),
    cmocka_unit_test_setup (InitializationDiagnostics,    Before),
    cmocka_unit_test_setup (FinalMeasurementBeforeClose,  Before),
    cmocka_unit_test_setup (DiagnosticsBeforeEbs,         Before),
    cmocka_unit_test_setup (CapabilityConsumer,           Before)
  };
  return cmocka_run_group_tests (Tests, NULL, NULL);
}
