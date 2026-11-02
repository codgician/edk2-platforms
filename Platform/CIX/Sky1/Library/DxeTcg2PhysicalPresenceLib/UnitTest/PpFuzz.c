/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <Library/BaseMemoryLib.h>
#include "PpMocks.h"
extern UINT32  mPpHashBitmap;

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
  UINT32   Operation;
  UINT32   Desired;
  UINT32   Hashable;
  BOOLEAN  AllocationValid;

  if (Size < 10) {
    return 0;
  }

  PpReset (0, 0);
  mPp.RequestSize      = Data[0] % (sizeof (mPp.Request) + 2);
  mPp.RequestAttr      = (Data[2] & 1) ? PP_ATTR : Data[2];
  mPp.Accept           = (Data[4] & 1) != 0;
  mPp.Timeout          = (Data[4] & 2) != 0;
  mPp.RequestRead      = (Data[5] & 1) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
  mPp.CapabilityStatus = (Data[5] & 4) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
  mPp.Supported        = Data[6] & 31U;
  mPp.Active           = Data[7] & mPp.Supported;
  if (Size > 12) {
    CopyMem (&mPp.Request, Data + 12, MIN (Size - 12, sizeof (mPp.Request)));
  }

  if (Size > 28) {
    mPpHashBitmap = Data[28];
  }

  Operation = mPp.Request.PPRequest;
  Hashable  = mPpHashBitmap;
  Desired   = Operation == 33 ? (mPp.Supported & Hashable) : mPp.Request.PPRequestParameter;
  AllocationValid = !EFI_ERROR (mPp.CapabilityStatus) && mPp.Active != 0 && Desired != 0 &&
                    (Desired & ~(mPp.Supported & Hashable)) == 0;
  Valid = ValidRequest () && mPp.RequestSize == sizeof (mPp.Request) &&
          mPp.RequestAttr == PP_ATTR && !EFI_ERROR (mPp.RequestRead);
  if (Operation == 23 || Operation == 33) {
    Valid = Valid && AllocationValid;
  }

  BOOLEAN Confirm = Tcg2PhysicalPresenceLibNeedUserConfirm ();
  if ((Operation == 23 || Operation == 33) && !AllocationValid) {
    assert (!Confirm);
  }
  Tcg2PhysicalPresenceLibReturnOperationResponseToOsFunction (&Recent, &Response);
  Tcg2PhysicalPresenceLibProcessRequest (NULL);
  if (!Valid || !mPp.Accept || mPp.Timeout) {
    assert (mPp.TraceCount == 0);
  }

  if (mPp.TraceCount) {
    assert (mPp.KeyReads == 1 && mPp.Request.PPRequest == 0 && mPp.Request.PPResponse == 0);
    assert (mPp.Trace[0] >= 1 && mPp.Trace[0] <= 4);
    if (mPp.Trace[0] == 4) {
      assert (AllocationValid && Desired != mPp.Active);
      assert (mPp.Trace[7] == Desired && (mPp.Trace[7] & ~Hashable) == 0);
    }
  }

  assert (mPp.TraceCount <= 2 && mPp.Resets <= 1);
  return 0;
}
