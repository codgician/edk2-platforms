/** @file
  Lock the TPM platform hierarchy before any OS loader runs.

  SecurityPkg's Tcg2PlatformDxe waits for gEfiDxeSmmReadyToLockProtocolGuid,
  which Sky1's BDS never installs. Use ReadyToBoot instead: it follows the
  physical-presence processing in PlatformBootManagerBeforeConsole, which may
  still need the empty platform authorization.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/TpmPlatformHierarchyLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

STATIC
VOID
EFIAPI
OnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  //
  // Randomize the platform authorization; later ReadyToBoot signals (boot
  // option retries) must not touch the hierarchy again.
  //
  ConfigureTpmPlatformHierarchy ();
  gBS->CloseEvent (Event);
}

EFI_STATUS
EFIAPI
TpmPlatformHierarchyDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_EVENT  Event;

  return EfiCreateEventReadyToBootEx (TPL_CALLBACK, OnReadyToBoot, NULL, &Event);
}
