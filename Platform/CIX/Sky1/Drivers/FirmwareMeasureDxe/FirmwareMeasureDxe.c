/** @file
  Measure the firmware volumes extracted from the boot firmware into PCR0.

  Sky1 has no PEI phase, so Tcg2Pei never measures the DXE firmware volume,
  and DxeTpm2MeasureBootLib deliberately skips images loaded from firmware
  volumes. Without this driver PCR0 holds only its separator, whatever
  firmware runs.

  PrePi publishes every volume it extracts from the FD as an FV2 HOB. Those
  copies are only read after extraction, so their bytes equal the build's FV
  files and the resulting PCR0 is predictable from the build output. The FD
  itself is not measured: PrePi executes in place from it and keeps writable
  globals there.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <PiDxe.h>
#include <Library/HobLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/TcgEventLogRecordLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

/** Emit a measurement decision independently of DEBUG output. **/
STATIC
VOID
FirmwareMeasureStatus (
  IN CONST CHAR8  *Decision,
  IN EFI_STATUS   Status
  )
{
  CHAR8  Line[128];
  UINTN  Length;

  Length = AsciiSPrint (Line, sizeof (Line), "TPM FirmwareMeasure: %a (%r)\r\n", Decision, Status);
  SerialPortWrite ((UINT8 *)Line, Length);
  if ((gST != NULL) && (gST->ConOut != NULL)) {
    Print (L"%a", Line);
  }
}

/**
  Extend PCR0 with every extracted firmware volume before BDS signals the
  PCR0 separator at ReadyToBoot.

  @param[in]  ImageHandle  Image handle of this driver.
  @param[in]  SystemTable  UEFI system table.

  @retval EFI_SUCCESS    Every extracted volume was measured.
  @retval EFI_NOT_FOUND  PrePi published no extracted volume.
  @return                The TCG2 measurement error. Tcg2Dxe has then already
                         capped PCRs 0-7 and reported its decision.
**/
EFI_STATUS
EFIAPI
FirmwareMeasureDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_PEI_HOB_POINTERS  Hob;
  EFI_STATUS            Status;
  UINTN                 Measured;

  Measured = 0;
  for (Hob.Raw = GetFirstHob (EFI_HOB_TYPE_FV2);
       Hob.Raw != NULL;
       Hob.Raw = GetNextHob (EFI_HOB_TYPE_FV2, GET_NEXT_HOB (Hob)))
  {
    Status = MeasureFirmwareBlob (
               0,
               NULL,
               Hob.FirmwareVolume2->BaseAddress,
               Hob.FirmwareVolume2->Length
               );
    if (EFI_ERROR (Status)) {
      FirmwareMeasureStatus ("firmware volume measurement failed", Status);
      return Status;
    }

    Measured++;
  }

  if (Measured == 0) {
    FirmwareMeasureStatus ("no extracted firmware volume, PCR0 not extended", EFI_NOT_FOUND);
    return EFI_NOT_FOUND;
  }

  return EFI_SUCCESS;
}
