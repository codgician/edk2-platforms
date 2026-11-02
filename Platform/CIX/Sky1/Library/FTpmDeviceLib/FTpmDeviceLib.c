/** @file
  TPM2 transport to the fTPM trusted application through FTPM_INIT_PROTOCOL.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>
#include <Library/Tpm2DeviceLib.h>
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
Tpm2SubmitCommand (
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
Tpm2RequestUseTpm (
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
Tpm2RegisterTpm2DeviceLib (
  IN TPM2_DEVICE_INTERFACE  *Tpm2Device
  )
{
  //
  // Only Tpm2DeviceLibRouter registers devices; this platform links the
  // transport directly.
  //
  return EFI_UNSUPPORTED;
}
