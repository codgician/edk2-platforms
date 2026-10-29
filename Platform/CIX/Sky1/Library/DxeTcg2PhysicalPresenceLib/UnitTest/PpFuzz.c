/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <Library/BaseMemoryLib.h>
#include "PpMocks.h"

static BOOLEAN
ValidRequest (
  VOID
  )
{
  UINT32  Op    = mPp.Request.PPRequest;
  UINT32  Param = mPp.Request.PPRequestParameter;

  if (Op == 23) {
    return Param != 0 && (Param & ~0x1fU) == 0;
  }

  return (Op == 0 || Op == 5 || Op == 14 || Op == 21 || Op == 22 || Op == 24 || Op == 33) && Param == 0;
}

int
LLVMFuzzerTestOneInput (
  const uint8_t  *Data,
  size_t         Size
  )
{
  UINT32   Recent, Response;
  BOOLEAN  Valid;

  if (Size < 10) {
    return 0;
  }

  PpReset (0, 0);
  mPp.RequestSize      = Data[0] % (sizeof (mPp.Request) + 2);
  mPp.FlagsSize        = Data[1] % (sizeof (mPp.Flags) + 2);
  mPp.RequestAttr      = (Data[2] & 1) ? PP_ATTR : Data[2];
  mPp.FlagsAttr        = (Data[3] & 1) ? PP_ATTR : Data[3];
  mPp.Accept           = (Data[4] & 1) != 0;
  mPp.Timeout          = (Data[4] & 2) != 0;
  mPp.PolicyAvailable  = (Data[4] & 4) != 0;
  mPp.PolicyEnabled    = (Data[4] & 8) != 0;
  mPp.Locked           = (Data[4] & 16) != 0;
  mPp.RequestRead      = (Data[5] & 1) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
  mPp.FlagsRead        = (Data[5] & 2) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
  mPp.CapabilityStatus = (Data[5] & 4) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
  mPp.Supported        = Data[6];
  mPp.Active           = Data[7];
  mPp.PolicyMismatch   = (Data[5] >> 3) % 10;
  CopyMem (&mPp.Flags, Data + 8, MIN (Size - 8, sizeof (mPp.Flags)));
  if (Size > 12) {
    CopyMem (&mPp.Request, Data + 12, MIN (Size - 12, sizeof (mPp.Request)));
  }

  Valid = ValidRequest () && mPp.RequestSize == sizeof (mPp.Request) && mPp.FlagsSize == sizeof (mPp.Flags) &&
          mPp.RequestAttr == PP_ATTR && mPp.FlagsAttr == PP_ATTR && (mPp.Flags.PPFlags & ~0x703f2U) == 0 &&
          mPp.PolicyAvailable && mPp.PolicyEnabled && mPp.Locked && mPp.PolicyMismatch == 0 && !EFI_ERROR (mPp.RequestRead) && !EFI_ERROR (mPp.FlagsRead);
  Tcg2PhysicalPresenceLibNeedUserConfirm ();
  Tcg2PhysicalPresenceLibReturnOperationResponseToOsFunction (&Recent, &Response);
  Tcg2PhysicalPresenceLibProcessRequest (NULL);
  if (!Valid || !mPp.Accept || mPp.Timeout) {
    assert (mPp.TraceCount == 0);
  }

  if (mPp.TraceCount) {
    assert (mPp.KeyReads == 1 && mPp.Request.PPRequest == 0 && mPp.Request.PPResponse == 0);
    assert (mPp.Trace[0] >= 1 && mPp.Trace[0] <= 4);
  }

  assert (mPp.TraceCount <= 2 && mPp.Resets <= 1);
  return 0;
}
