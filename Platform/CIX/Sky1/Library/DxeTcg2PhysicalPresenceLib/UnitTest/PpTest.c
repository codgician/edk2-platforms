#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include "PpMocks.h"
#include "HostSupport.h"

static void
log_all_rejected (
  void  **State
  )
{
  PpReset (TCG2_PHYSICAL_PRESENCE_LOG_ALL_DIGESTS, 0);
  assert_true (Tcg2PhysicalPresenceLibNeedUserConfirm ());
}

static void
short_variable (
  void  **State
  )
{
  PpReset (TCG2_PHYSICAL_PRESENCE_CLEAR, 0);
  mPp.RequestSize--;
  assert_false (Tcg2PhysicalPresenceLibNeedUserConfirm ());
}

static void
wrong_attributes (
  void  **State
  )
{
  PpReset (TCG2_PHYSICAL_PRESENCE_CLEAR, 0);
  mPp.RequestAttr = EFI_VARIABLE_BOOTSERVICE_ACCESS;
  assert_false (Tcg2PhysicalPresenceLibNeedUserConfirm ());
}

static void
lock_missing (
  void  **State
  )
{
  PpReset (TCG2_PHYSICAL_PRESENCE_CLEAR, 0);
  mPp.PolicyAvailable = FALSE;
  assert_false (Tcg2PhysicalPresenceLibNeedUserConfirm ());
}

static void
reserved_flags (
  void  **State
  )
{
  PpReset (TCG2_PHYSICAL_PRESENCE_CLEAR, 0);
  mPp.Flags.PPFlags |= BIT0;
  assert_false (Tcg2PhysicalPresenceLibNeedUserConfirm ());
}

static void
bad_parameter (
  void  **State
  )
{
  PpReset (TCG2_PHYSICAL_PRESENCE_CLEAR, 1);
  assert_false (Tcg2PhysicalPresenceLibNeedUserConfirm ());
}

static void
no_waiver (
  void  **State
  )
{
  PpReset (TCG2_PHYSICAL_PRESENCE_CLEAR, 0);
  mPp.Flags.PPFlags = 0;
  assert_true (Tcg2PhysicalPresenceLibNeedUserConfirm ());
}

static void
schemas (
  void  **State
  )
{
  UINTN   N;
  UINT32  Recent, Response;

  for (N = 0; N <= sizeof (mPp.Request) + 1; ++N) {
    PpReset (5, 0);
    mPp.Accept      = TRUE;
    mPp.RequestSize = N;
    Tcg2PhysicalPresenceLibProcessRequest (NULL);
    assert_int_equal (mPp.TraceCount, N == sizeof (mPp.Request) ? 2 : 0);
    if (N != sizeof (mPp.Request)) {
      assert_int_equal (mPp.Writes, 0);
      assert_int_equal (Tcg2PhysicalPresenceLibReturnOperationResponseToOsFunction (&Recent, &Response), 1);
      assert_int_equal (Recent, 0);
      assert_int_equal (Response, 0);
    }
  }

  for (N = 0; N <= sizeof (mPp.Flags) + 1; ++N) {
    PpReset (5, 0);
    mPp.Accept    = TRUE;
    mPp.FlagsSize = N;
    Tcg2PhysicalPresenceLibProcessRequest (NULL);
    assert_int_equal (mPp.TraceCount, N == sizeof (mPp.Flags) ? 2 : 0);
    PpReset (5, 0);
    mPp.Locked    = FALSE;
    mPp.FlagsSize = N;
    assert_int_equal (TPMVariableDxeEntryPoint (NULL, NULL), N == sizeof (mPp.Flags) ? EFI_SUCCESS : EFI_COMPROMISED_DATA);
    assert_int_equal (mPp.Writes, 0);
  }

  for (N = 0; N < 32; ++N) {
    PpReset (5, 0);
    mPp.Accept      = TRUE;
    mPp.RequestAttr = 1U << N;
    Tcg2PhysicalPresenceLibProcessRequest (NULL);
    assert_int_equal (mPp.TraceCount, 0);
    assert_int_equal (mPp.Writes, 0);
    PpReset (5, 0);
    mPp.Accept    = TRUE;
    mPp.FlagsAttr = PP_ATTR | (1U << N);
    Tcg2PhysicalPresenceLibProcessRequest (NULL);
    assert_int_equal (mPp.TraceCount, N < 3 ? 2 : 0);
  }
}

static void
operations (
  void  **State
  )
{
  UINT32  Op, Mode;

  for (Op = 0; Op < 256; ++Op) {
    for (Mode = 0; Mode < 3; ++Mode) {
      BOOLEAN  Supported = Op == 0 || Op == 5 || Op == 14 || Op == 21 || Op == 22 || Op == 23 || Op == 24 || Op == 33;
      PpReset ((UINT8)Op, Op == 23 ? 2 : 0);
      mPp.Accept  = Mode == 1;
      mPp.Timeout = Mode == 2;
      Tcg2PhysicalPresenceLibProcessRequest (NULL);
      if (!Supported || (Op == 0)) {
        assert_int_equal (mPp.TraceCount, 0);
        assert_int_equal (mPp.KeyReads, 0);
        assert_int_equal (mPp.Writes, 0);
      } else if (Mode != 1) {
        assert_int_equal (mPp.TraceCount, 0);
        assert_int_equal (mPp.Resets, 0);
        assert_int_equal (mPp.Request.PPResponse, TCG_PP_OPERATION_RESPONSE_USER_ABORT);
      } else {
        assert_int_equal (mPp.TraceCount, (Op == 23 || Op == 24 || Op == 33) ? 1 : 2);
        assert_int_equal (mPp.Trace[0], Op == 24 ? 3 : (Op == 23 || Op == 33) ? 4 : 1);
        if (mPp.TraceCount == 2) {
          assert_int_equal (mPp.Trace[1], 2);
        }

        if ((Op == 23) || (Op == 33)) {
          assert_int_equal (mPp.Trace[7], Op == 23 ? 2 : 15);
        }

        assert_int_equal (mPp.Resets, 1);
        assert_int_equal (mPp.KeyReads, 1);
        assert_int_equal (mPp.Request.PPRequest, 0);
        assert_int_equal (mPp.Request.PPResponse, 0);
      }
    }
  }

  PpReset (33, 0);
  mPp.Active = mPp.Supported;
  Tcg2PhysicalPresenceLibProcessRequest (NULL);
  assert_int_equal (mPp.TraceCount, 0);
  assert_int_equal (mPp.KeyReads, 0);
  assert_int_equal (mPp.Resets, 0);
  assert_int_equal (mPp.Request.PPResponse, 0);
  PpReset (23, 6);
  Tcg2PhysicalPresenceLibProcessRequest (NULL);
  assert_int_equal (mPp.TraceCount, 0);
  assert_int_equal (mPp.KeyReads, 0);
  assert_int_equal (mPp.Resets, 0);
}

static void
failures (
  void  **State
  )
{
  EFI_STATUS  Errors[] = { EFI_BUFFER_TOO_SMALL, EFI_NOT_FOUND, EFI_DEVICE_ERROR, EFI_SECURITY_VIOLATION };
  UINTN       I;

  for (I = 0; I < sizeof (Errors) / sizeof (Errors[0]); ++I) {
    PpReset (5, 0);
    mPp.Accept      = TRUE;
    mPp.RequestRead = Errors[I];
    Tcg2PhysicalPresenceLibProcessRequest (NULL);
    assert_int_equal (mPp.TraceCount, 0);
    PpReset (5, 0);
    mPp.Accept    = TRUE;
    mPp.FlagsRead = Errors[I];
    Tcg2PhysicalPresenceLibProcessRequest (NULL);
    assert_int_equal (mPp.TraceCount, 0);
    PpReset (5, 0);
    mPp.Locked     = FALSE;
    mPp.LockStatus = Errors[I];
    assert_int_equal (TPMVariableDxeEntryPoint (NULL, NULL), Errors[I]);
    mPp.Accept = TRUE;
    Tcg2PhysicalPresenceLibProcessRequest (NULL);
    assert_int_equal (mPp.TraceCount, 0);
  }

  PpReset (5, 0);
  mPp.Accept      = TRUE;
  mPp.WriteStatus = EFI_DEVICE_ERROR;
  Tcg2PhysicalPresenceLibProcessRequest (NULL);
  assert_int_equal (mPp.TraceCount, 0);
  PpReset (5, 0);
  mPp.Accept        = TRUE;
  mPp.CommandStatus = EFI_DEVICE_ERROR;
  Tcg2PhysicalPresenceLibProcessRequest (NULL);
  assert_int_equal (mPp.TraceCount, 1);
  assert_int_equal (mPp.Trace[0], 1);
  assert_int_equal (mPp.Request.PPResponse, TCG_PP_OPERATION_RESPONSE_BIOS_FAILURE);
  assert_int_equal (mPp.Resets, 0);
  PpReset (33, 0);
  mPp.Accept           = TRUE;
  mPp.CapabilityStatus = EFI_DEVICE_ERROR;
  Tcg2PhysicalPresenceLibProcessRequest (NULL);
  assert_int_equal (mPp.TraceCount, 0);
  assert_int_equal (mPp.KeyReads, 0);
  PpReset (5, 0);
  mPp.Accept        = TRUE;
  mPp.PolicyEnabled = FALSE;
  Tcg2PhysicalPresenceLibProcessRequest (NULL);
  assert_int_equal (mPp.TraceCount, 0);
  assert_int_equal (mPp.Writes, 0);
  PpReset (5, 0);
  mPp.Accept = TRUE;
  mPp.Locked = FALSE;
  Tcg2PhysicalPresenceLibProcessRequest (NULL);
  assert_int_equal (mPp.TraceCount, 0);
  assert_int_equal (mPp.Writes, 0);
}

static void
submit (
  void  **State
  )
{
  PpReset (0, 0);
  assert_int_equal (Tcg2PhysicalPresenceLibSubmitRequestToPreOSFunction (5, 0), 0);
  assert_int_equal (mPp.Request.PPRequest, 5);
  assert_int_equal (mPp.Request.PPRequestParameter, 0);
  assert_int_equal (mPp.TraceCount, 0);
  assert_int_equal (Tcg2PhysicalPresenceLibSubmitRequestToPreOSFunction (0x105, 0), 1);
  assert_int_equal (Tcg2PhysicalPresenceLibSubmitRequestToPreOSFunction (5, 1), 1);
  assert_int_equal (Tcg2PhysicalPresenceLibSubmitRequestToPreOSFunction (23, 0), 1);
  assert_int_equal (Tcg2PhysicalPresenceLibSubmitRequestToPreOSFunction (23, 0x80000000), 1);
  assert_int_equal (mPp.Writes, 1);
  mPp.RequestSize = 1;
  assert_int_equal (Tcg2PhysicalPresenceLibSubmitRequestToPreOSFunction (5, 0), 2);
  assert_int_equal (mPp.Writes, 1);
}

static void
history_and_pcr_errors (
  void  **State
  )
{
  UINT32  Masks[] = { 0, 0x10, 0x20, 0xffffffff };
  UINTN   I;

  PpReset (5, 0);
  mPp.Accept                = TRUE;
  mPp.Request.LastPPRequest = 0xff;
  Tcg2PhysicalPresenceLibProcessRequest (NULL);
  assert_int_equal (mPp.TraceCount, 0);
  assert_int_equal (mPp.Writes, 0);
  PpReset (5, 0);
  mPp.Accept             = TRUE;
  mPp.Request.PPResponse = 1;
  Tcg2PhysicalPresenceLibProcessRequest (NULL);
  assert_int_equal (mPp.TraceCount, 0);
  assert_int_equal (mPp.Writes, 0);
  for (I = 0; I < sizeof (Masks) / sizeof (Masks[0]); ++I) {
    PpReset (23, Masks[I]);
    mPp.Accept = TRUE;
    Tcg2PhysicalPresenceLibProcessRequest (NULL);
    assert_int_equal (mPp.TraceCount, 0);
    assert_int_equal (mPp.KeyReads, 0);
  }

  PpReset (33, 0);
  mPp.Accept    = TRUE;
  mPp.Supported = 0;
  Tcg2PhysicalPresenceLibProcessRequest (NULL);
  assert_int_equal (mPp.TraceCount, 0);
  PpReset (33, 0);
  mPp.Accept = TRUE;
  mPp.Active = 0x80;
  Tcg2PhysicalPresenceLibProcessRequest (NULL);
  assert_int_equal (mPp.TraceCount, 0);
  PpReset (5, 0);
  mPp.Flags.PPFlags = BIT3;
  mPp.Accept        = TRUE;
  Tcg2PhysicalPresenceLibProcessRequest (NULL);
  assert_int_equal (mPp.TraceCount, 0);
  PpReset (0, 0);
  mPp.Locked    = FALSE;
  mPp.FlagsRead = EFI_NOT_FOUND;
  assert_int_equal (TPMVariableDxeEntryPoint (NULL, NULL), EFI_SUCCESS);
  assert_true (mPp.Locked);
  assert_int_equal (mPp.Writes, 1);
  PpReset (0, 0);
  mPp.Locked    = FALSE;
  mPp.FlagsRead = EFI_DEVICE_ERROR;
  assert_int_equal (TPMVariableDxeEntryPoint (NULL, NULL), EFI_DEVICE_ERROR);
  assert_int_equal (mPp.Writes, 0);
}

static void
console_and_lock_errors (
  void  **State
  )
{
  UINT32  I;

  for (I = 1; I <= 9; ++I) {
    PpReset (5, 0);
    mPp.Accept         = TRUE;
    mPp.PolicyMismatch = I;
    Tcg2PhysicalPresenceLibProcessRequest (NULL);
    assert_int_equal (mPp.TraceCount, 0);
    assert_int_equal (mPp.Writes, 0);
  }

  PpReset (5, 0);
  mPp.Accept        = TRUE;
  mPp.ConsoleStatus = EFI_DEVICE_ERROR;
  Tcg2PhysicalPresenceLibProcessRequest (NULL);
  assert_int_equal (mPp.TraceCount, 0);
  PpReset (5, 0);
  mPp.Accept       = TRUE;
  mPp.PrintFailure = TRUE;
  Tcg2PhysicalPresenceLibProcessRequest (NULL);
  assert_int_equal (mPp.TraceCount, 0);
  PpReset (5, 0);
  mPp.Accept            = TRUE;
  mPp.AllocationFailure = TRUE;
  Tcg2PhysicalPresenceLibProcessRequest (NULL);
  assert_int_equal (mPp.TraceCount, 0);
  assert_int_equal (mPp.Writes, 0);
  assert_int_equal (HostLivePoolAllocations (), 0);
}

static void
consume_before_mutation (
  void  **State
  )
{
  PpReset (5, 0);
  mPp.Accept          = TRUE;
  mPp.FailWriteNumber = 2;
  Tcg2PhysicalPresenceLibProcessRequest (NULL);
  assert_int_equal (mPp.TraceCount, 2);
  assert_int_equal (mPp.Request.PPRequest, 0);
  assert_int_equal (mPp.Request.PPResponse, TCG_PP_OPERATION_RESPONSE_BIOS_FAILURE);
  Tcg2PhysicalPresenceLibProcessRequest (NULL);
  assert_int_equal (mPp.TraceCount, 2);
  assert_int_equal (mPp.KeyReads, 1);
}

int
main (
  void
  )
{
  const struct CMUnitTest  Tests[] = {
    cmocka_unit_test (short_variable),
    cmocka_unit_test (wrong_attributes),
    cmocka_unit_test (lock_missing),
    cmocka_unit_test (reserved_flags),
    cmocka_unit_test (bad_parameter),
    cmocka_unit_test (no_waiver),
    cmocka_unit_test (log_all_rejected),
    cmocka_unit_test (schemas),
    cmocka_unit_test (operations),
    cmocka_unit_test (failures),
    cmocka_unit_test (submit),
    cmocka_unit_test (history_and_pcr_errors),
    cmocka_unit_test (console_and_lock_errors),
    cmocka_unit_test (consume_before_mutation)
  };

  return cmocka_run_group_tests (Tests, NULL, NULL);
}
