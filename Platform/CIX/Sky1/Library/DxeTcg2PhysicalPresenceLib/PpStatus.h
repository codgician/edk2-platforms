/** @file
  Non-secret, RELEASE-visible physical-presence diagnostics.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef SKY1_PP_STATUS_H_
#define SKY1_PP_STATUS_H_

#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>

/** Report why physical-presence processing is unavailable without blocking boot.

  Use SerialPortLib rather than DEBUG: RELEASE disables DEBUG, and vendor debug
  output can expose entropy. The serial path also works before ConOut exists.

  @param[in] Reason  Static, non-secret description of the skipped step.
  @param[in] Status Failure status, never variable contents or authorization.
  @return           Status, unchanged.
**/
STATIC
EFI_STATUS
PpUnavailable (
  IN CONST CHAR8  *Reason,
  IN EFI_STATUS   Status
  )
{
  CHAR8  Line[192];
  UINTN  Length;

  Length = AsciiSPrint (Line, sizeof (Line), "TPM PP: %a, skipping requests (%r)\r\n", Reason, Status);
  SerialPortWrite ((UINT8 *)Line, Length);
  if ((gST != NULL) && (gST->ConOut != NULL)) {
    Print (L"%a", Line);
  }

  return Status;
}

#endif
