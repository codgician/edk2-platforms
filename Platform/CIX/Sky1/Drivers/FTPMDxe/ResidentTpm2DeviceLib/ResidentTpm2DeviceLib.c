/** @file
  Private TPM transport for capping before FTPM_INIT_PROTOCOL publication.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Uefi.h>
#include <Library/Tpm2DeviceLib.h>

BOOLEAN
EFIAPI
FtpmResidentReady (
  VOID
  );

EFI_STATUS
EFIAPI
FtpmResidentSubmitCommand (
  IN UINT32      InputSize,
  IN UINT8       *Input,
  IN OUT UINT32  *OutputSize,
  OUT UINT8      *Output
  );

EFI_STATUS
EFIAPI
Tpm2SubmitCommand (
  IN UINT32      InputParameterBlockSize,
  IN UINT8       *InputParameterBlock,
  IN OUT UINT32  *OutputParameterBlockSize,
  OUT UINT8      *OutputParameterBlock
  )
{
  return FtpmResidentSubmitCommand (
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
  return FtpmResidentReady () ? EFI_SUCCESS : EFI_NOT_FOUND;
}
