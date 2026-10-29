/** @file
  TPM2 transport to the fTPM trusted application through FTPM_INIT_PROTOCOL.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/FTpmDeviceLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/FTPMInitProtocol.h>

STATIC FTPM_INIT_PROTOCOL  *mFtpm;

STATIC
FTPM_INIT_PROTOCOL *
GetFtpm (
  VOID
  )
{
  if (mFtpm == NULL) {
    gBS->LocateProtocol (&gCixFtpmInitProtocolGuid, NULL, (VOID **)&mFtpm);
  }

  return mFtpm;
}

EFI_STATUS
EFIAPI
FTpmSubmitCommand (
  IN UINT32      InputParameterBlockSize,
  IN UINT8       *InputParameterBlock,
  IN OUT UINT32  *OutputParameterBlockSize,
  IN UINT8       *OutputParameterBlock
  )
{
  FTPM_INIT_PROTOCOL  *Ftpm;

  Ftpm = GetFtpm ();
  if (Ftpm == NULL) {
    return EFI_NOT_FOUND;
  }

  return Ftpm->SubmitCommand (
                 Ftpm,
                 InputParameterBlockSize,
                 InputParameterBlock,
                 OutputParameterBlockSize,
                 OutputParameterBlock
                 );
}

EFI_STATUS
EFIAPI
FTpmRequestUseTpm (
  VOID
  )
{
  FTPM_INIT_PROTOCOL  *Ftpm;

  Ftpm = GetFtpm ();
  if ((Ftpm == NULL) || !Ftpm->GetFTPMStatus (Ftpm)) {
    return EFI_NOT_FOUND;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
FTpmRegisterTpm2DeviceLib (
  VOID
  )
{
  //
  // Only Tpm2DeviceLibRouter registers devices; this platform links the
  // transport directly.
  //
  return EFI_UNSUPPORTED;
}
