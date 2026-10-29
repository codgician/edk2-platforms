/** SPDX-License-Identifier: BSD-2-Clause-Patent **/
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>
#include <Library/RngLib.h>
#include "RngMock.h"

STATIC CONST UINTN mWidths[] = { 2, 4, 8, 16 };
STATIC BOOLEAN
CallRng (UINTN Width, UINT64 *Buffer)
{
  switch (Width) {
    case 2: return GetRandomNumber16 ((UINT16 *)Buffer);
    case 4: return GetRandomNumber32 ((UINT32 *)Buffer);
    case 8: return GetRandomNumber64 (Buffer);
    case 16: return GetRandomNumber128 (Buffer);
    default: fail (); return FALSE;
  }
}

STATIC VOID
CheckResult (UINTN Width, BOOLEAN Expected, BOOLEAN Scrub)
{
  UINT64 Buffer[4];
  UINT8 Before[sizeof (Buffer)];
  BOOLEAN Result;
  memset (Buffer, 0xA5, sizeof (Buffer));
  memcpy (Before, Buffer, sizeof (Buffer));
  Result = CallRng (Width, &Buffer[1]);
  assert_int_equal (Result, Expected);
  assert_memory_equal (Buffer, Before, sizeof (UINT64));
  assert_memory_equal ((UINT8 *)&Buffer[1] + Width, Before + 8 + Width, sizeof (Buffer) - 8 - Width);
  if (Expected) {
    assert_memory_equal (&Buffer[1], gReply.Data, Width);
  } else {
    // Failed output must not publish bytes from a malformed/stale response.
    assert_memory_equal (&Buffer[1], Before + 8, Width);
  }
  CheckRngMock (Scrub);
}

STATIC VOID
ValidWidths (VOID **State)
{
  UINTN Index;
  (VOID)State;
  for (Index = 0; Index < 4; Index++) {
    ResetRngMock (mWidths[Index]);
    CheckResult (mWidths[Index], TRUE, FALSE);
    ResetRngMock (mWidths[Index]);
    memset (gReply.Data, 0, sizeof (gReply.Data));
    CheckResult (mWidths[Index], TRUE, FALSE);
    ResetRngMock (mWidths[Index]);
    memset (gReply.Data, 0xFF, sizeof (gReply.Data));
    CheckResult (mWidths[Index], TRUE, FALSE);
  }
}

STATIC VOID
MalformedLengths (VOID **State)
{
  UINTN Index;
  UINTN Case;
  UINT32 Lengths[6];
  (VOID)State;
  for (Index = 0; Index < 4; Index++) {
    Lengths[0] = 0;
    Lengths[1] = (UINT32)mWidths[Index] - 1;
    Lengths[2] = (UINT32)mWidths[Index] + 1;
    Lengths[3] = MAX_TRNG_SIZE;
    Lengths[4] = MAX_TRNG_SIZE + 1;
    Lengths[5] = MAX_UINT32;
    for (Case = 0; Case < 6; Case++) {
      ResetRngMock (mWidths[Index]);
      gReply.Length = Lengths[Case];
      CheckResult (mWidths[Index], FALSE, FALSE);
    }
  }
}

STATIC VOID
MailboxErrors (VOID **State)
{
  UINTN Index;
  (VOID)State;
  for (Index = 0; Index < 4; Index++) {
    ResetRngMock (mWidths[Index]);
    gReply.Status = EFI_DEVICE_ERROR;
    CheckResult (mWidths[Index], FALSE, FALSE);
    ResetRngMock (mWidths[Index]);
    gReply.Error = MAX_UINT32;
    CheckResult (mWidths[Index], FALSE, FALSE);
  }
}

STATIC VOID
CommandMismatchAndStale (VOID **State)
{
  UINTN Index;
  (VOID)State;
  for (Index = 0; Index < 4; Index++) {
    ResetRngMock (mWidths[Index]);
    CheckResult (mWidths[Index], TRUE, FALSE);
    ResetRngMock (mWidths[Index]);
    // Audited MboxGetTrng leaves the caller response untouched on mismatch.
    gReply.Unchanged = TRUE;
    CheckResult (mWidths[Index], FALSE, FALSE);
  }
}

STATIC VOID
ResponseScrubbing (VOID **State)
{
  UINTN Index;
  UINTN Case;
  (VOID)State;
  for (Index = 0; Index < 4; Index++) {
    for (Case = 0; Case < 5; Case++) {
      ResetRngMock (mWidths[Index]);
      if (Case == 1) { gReply.Status = EFI_DEVICE_ERROR; }
      if (Case == 2) { gReply.Error = 1; }
      if (Case == 3) { gReply.Length = 0; }
      if (Case == 4) { gReply.Unchanged = TRUE; }
      CheckResult (mWidths[Index], Case == 0, TRUE);
    }
  }
}

STATIC VOID
FreshSentinel (VOID **State)
{
  UINTN Index;
  (VOID)State;
  for (Index = 0; Index < 4; Index++) {
    ResetRngMock (mWidths[Index]);
    gCheckSentinel = TRUE;
    CheckResult (mWidths[Index], TRUE, TRUE);
  }
}

STATIC VOID
NullAsserts (VOID **State)
{
  UINTN Index;
#if !defined (MDEPKG_NDEBUG)
  int Status;
  pid_t Child;
#endif
  (VOID)State;
  for (Index = 0; Index < 4; Index++) {
    ResetRngMock (0);
#if defined (MDEPKG_NDEBUG)
    assert_false (CallRng (mWidths[Index], NULL));
#else
    Child = fork ();
    assert_true (Child >= 0);
    if (Child == 0) {
      CallRng (mWidths[Index], NULL);
      _exit (99);
    }
    assert_int_equal (waitpid (Child, &Status, 0), Child);
    assert_true (WIFSIGNALED (Status));
    assert_int_equal (WTERMSIG (Status), SIGABRT);
#endif
    assert_int_equal (gMailboxCalls, 0);
  }
}

int main (void)
{
  const struct CMUnitTest Tests[] = {
    cmocka_unit_test (ValidWidths),
    cmocka_unit_test (MalformedLengths),
    cmocka_unit_test (MailboxErrors),
    cmocka_unit_test (CommandMismatchAndStale),
    cmocka_unit_test (ResponseScrubbing),
    cmocka_unit_test (FreshSentinel),
    cmocka_unit_test (NullAsserts)
  };
  return cmocka_run_group_tests (Tests, NULL, NULL);
}
