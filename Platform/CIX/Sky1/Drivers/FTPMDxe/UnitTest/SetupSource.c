/** @file
  Compile the real setup-routing library with controllable fixed PCDs.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Uefi.h>
#include <PlatformSetupVar.h>

extern EFI_GUID  gPlatformSetupVariableGuid;
extern BOOLEAN   mSetupDtpSupported;
extern BOOLEAN   mSetupFtpSupported;
extern UINT8     mSetupDefault;

#define _PCD_VALUE_PcdDTPMSupport             mSetupDtpSupported
#define _PCD_VALUE_PcdFTPMSupport             mSetupFtpSupported
#define _PCD_VALUE_PcdDefaultTpmDeviceSelect  mSetupDefault

#include "../../../Library/Tpm2DeviceLib/Tpm2DeviceLib.c"

VOID
SetupResetSelection (
  VOID
  )
{
  TPMDeviceSelect = 255;
}
