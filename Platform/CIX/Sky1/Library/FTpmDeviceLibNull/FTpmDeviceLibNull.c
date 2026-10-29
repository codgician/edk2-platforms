/** @file
  FTpmDeviceLib for platforms without a FTpm TPM: every request fails so the
  CIX Tpm2DeviceLib dispatcher falls back or reports no TPM.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/FTpmDeviceLib.h>

EFI_STATUS
EFIAPI
FTpmSubmitCommand (
  IN UINT32      InputParameterBlockSize,
  IN UINT8       *InputParameterBlock,
  IN OUT UINT32  *OutputParameterBlockSize,
  IN UINT8       *OutputParameterBlock
  )
{
  return EFI_UNSUPPORTED;
}

EFI_STATUS
EFIAPI
FTpmRequestUseTpm (
  VOID
  )
{
  return EFI_NOT_FOUND;
}

EFI_STATUS
EFIAPI
FTpmRegisterTpm2DeviceLib (
  VOID
  )
{
  return EFI_UNSUPPORTED;
}
