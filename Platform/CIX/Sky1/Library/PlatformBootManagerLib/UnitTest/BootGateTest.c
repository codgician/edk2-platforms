/** @file Boot gate functional trace against the real platform BDS hook.
   SPDX-License-Identifier: BSD-2-Clause-Patent
 **/
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <cmocka.h>
#include <PiDxe.h>
extern EFI_GUID gPlatformSetupVariableGuid;
extern EFI_GUID gCixFastBootAppGuid;
extern EFI_GUID gBootDiscoveryPolicyMgrFormsetGuid;
extern EFI_GUID gCixFarmEnableFlagGuid;
extern EFI_GUID gCixExitConsoleCallBackGuid;
extern EFI_GUID gCixAndroidLoaderGuid;
extern EFI_GUID gCixLinuxBootSelectGuid;
extern EFI_GUID gUefiShellFileGuid;
extern EFI_GUID gCixAbselectGuid;
#include <Protocol/Tcg2Protocol.h>
#include <Protocol/SerialIo.h>
#include <Library/Tpm2CommandLib.h>
#include <Library/TpmPlatformHierarchyLib.h>
#include "../PlatformBm.c"

EFI_BOOT_SERVICES *   gBS;
EFI_RUNTIME_SERVICES *gRT;
EFI_SYSTEM_TABLE *    gST;
#define GUID(Name, Id) EFI_GUID Name = { Id, 0, 0, { 0 } }
GUID (gEfiEndOfDxeEventGroupGuid, 1);
GUID (gEfiPciRootBridgeIoProtocolGuid, 2);
GUID (gEfiPciIoProtocolGuid, 3);
GUID (gEfiGraphicsOutputProtocolGuid, 4);
GUID (gEdkiiNonDiscoverableDeviceProtocolGuid, 5);
GUID (gEdkiiNonDiscoverableUhciDeviceGuid, 6);
GUID (gEdkiiNonDiscoverableEhciDeviceGuid, 7);
GUID (gEdkiiNonDiscoverableXhciDeviceGuid, 8);
GUID (gEfiTtyTermGuid, 9);
GUID (gEfiSimpleTextInProtocolGuid, 10);
GUID (gEfiSimpleTextOutProtocolGuid, 11);
GUID (gEfiDevicePathProtocolGuid, 12);
GUID (gPlatformBootManagerProtocolGuid, 13);
GUID (gEfiTcg2ProtocolGuid, 14);
GUID (gEfiSerialIoProtocolGuid, 15);

STATIC jmp_buf                         mStop;
STATIC EFI_STATUS                      mHierarchyStatus;
STATIC EFI_STATUS                      mDisableStatus;
STATIC EFI_STATUS                      mCapStatus;
STATIC EFI_STATUS                      mConsoleStatus;
STATIC EFI_STATUS mConnectStatus;
STATIC BOOLEAN                         mTpmAbsent;
STATIC UINTN                           mPpCount;
STATIC UINTN                           mProtectCount;
STATIC UINTN                           mDisableCount;
STATIC UINTN                           mCapCount;
STATIC UINTN                           mWarnings;
STATIC BOOLEAN                         mStopped;
STATIC EFI_SIMPLE_TEXT_INPUT_PROTOCOL  mInput;
STATIC EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL mOutput;
STATIC EFI_BOOT_SERVICES               mBootServices;
STATIC EFI_SYSTEM_TABLE                mSystemTable;
STATIC CHAR8                           mTrace[128];
STATIC UINTN                           mTraceSize;

STATIC VOID
Trace (
  CHAR8 Ch
  )
{
  mTrace[mTraceSize++] = Ch;
  mTrace[mTraceSize]   = 0;
}
VOID EFIAPI
CpuDeadLoop (
  VOID
  )
{
  mStopped = TRUE;
  Trace ('!');
  longjmp (mStop, 1);
}
EFI_STATUS EFIAPI
EfiEventGroupSignal (
  CONST EFI_GUID *Guid
  )
{
  assert_ptr_equal (Guid, &gEfiEndOfDxeEventGroupGuid);
  Trace ('E');
  assert_int_equal (mPpCount, mTpmAbsent ? 0 : 1);
  assert_int_equal (mProtectCount, mTpmAbsent ? 0 : 1);
  if (EFI_ERROR (mHierarchyStatus)) {
    assert_int_equal (mDisableCount, 1);
    if (EFI_ERROR (mDisableStatus)) {
      assert_int_equal (mCapCount, 8);
      assert_true (mWarnings > 0);
    }
  }
  return EFI_SUCCESS;
}
VOID EFIAPI
Tcg2PhysicalPresenceLibProcessRequest (
  TPM2B_AUTH *Auth
  )
{
  assert_null (Auth);
  assert_ptr_equal (gST->ConIn, &mInput);
  assert_ptr_equal (gST->ConOut, &mOutput);
  mPpCount++;
  Trace ('P');
}
EFI_STATUS EFIAPI
ConfigureTpmPlatformHierarchy (
  VOID
  )
{
  assert_int_equal (mPpCount, 1);
  mProtectCount++;
  Trace ('H');
  return mHierarchyStatus;
}
EFI_STATUS EFIAPI
Tpm2HierarchyControl (
  TPMI_RH_HIERARCHY Auth,
  TPMS_AUTH_COMMAND *Session,
  TPMI_RH_HIERARCHY Hierarchy,
  TPMI_YES_NO State
  )
{
  assert_int_equal (Auth, TPM_RH_PLATFORM);
  assert_null (Session);
  assert_int_equal (Hierarchy, TPM_RH_PLATFORM);
  assert_int_equal (State, NO);
  mDisableCount++;
  Trace ('D');
  return mDisableStatus;
}
STATIC EFI_STATUS EFIAPI
Cap (
  EFI_TCG2_PROTOCOL *This,
  UINT64 Flags,
  EFI_PHYSICAL_ADDRESS Data,
  UINT64 Size,
  EFI_TCG2_EVENT *Event
  )
{
  UINT32 Value;
  assert_int_equal (Flags, EFI_TCG2_EXTEND_ONLY);
  assert_int_equal (Size, 4);
  CopyMem (&Value, (VOID *)(UINTN)Data, 4);
  assert_int_equal (Value, 1);
  assert_int_equal (Event->Header.EventType, EV_SEPARATOR);
  assert_int_equal (Event->Header.PCRIndex, mCapCount);
  mCapCount++;
  Trace ('C');
  return mCapStatus;
}
STATIC EFI_STATUS EFIAPI
Capability (
  EFI_TCG2_PROTOCOL *This,
  EFI_TCG2_BOOT_SERVICE_CAPABILITY *Cap
  )
{
  Cap->TPMPresentFlag = TRUE;
  return EFI_SUCCESS;
}
STATIC EFI_TCG2_PROTOCOL mTcg2 = {
  .GetCapability = Capability, .HashLogExtendEvent = Cap
};
STATIC EFI_STATUS EFIAPI
LocateProtocol (
  EFI_GUID *Guid,
  VOID *Registration,
  VOID **Interface
  )
{
  assert_null (Registration);
  if (Guid == &gEfiTcg2ProtocolGuid) {
    *Interface = &mTcg2;
    return mTpmAbsent ? EFI_NOT_FOUND : EFI_SUCCESS;
  }
  assert_ptr_equal (Guid, &gPlatformBootManagerProtocolGuid);
  return EFI_NOT_FOUND;
}
STATIC EFI_STATUS EFIAPI
LocateDevicePath (
  EFI_GUID *Guid,
  EFI_DEVICE_PATH_PROTOCOL **Path,
  EFI_HANDLE *Handle
  )
{
  if (EFI_ERROR (mConsoleStatus)) {
    return mConsoleStatus;
  }
  if (Guid == &gEfiSerialIoProtocolGuid) {
    assert_memory_equal (*Path, &mSerialConsole, OFFSET_OF (PLATFORM_SERIAL_CONSOLE, TermType));
    *Path   = &mSerialConsole.TermType.Header;
    *Handle = (EFI_HANDLE)(UINTN)1;
  }
  else {
    assert_true ((Guid == &gEfiSimpleTextInProtocolGuid) || (Guid == &gEfiSimpleTextOutProtocolGuid));
    *Path   = &mSerialConsole.End;
    *Handle = (EFI_HANDLE)(UINTN)2;
  }
  Trace ('S');
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI
HandleProtocol (
  EFI_HANDLE Handle,
  EFI_GUID *Guid,
  VOID **Interface
  )
{
  assert_ptr_equal (Handle, (EFI_HANDLE)(UINTN)2);
  if (Guid == &gEfiSimpleTextInProtocolGuid) {
    *Interface = &mInput;
  }
  else {
    assert_ptr_equal (Guid, &gEfiSimpleTextOutProtocolGuid);
    *Interface = &mOutput;
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI
ConnectController (
  EFI_HANDLE Handle,
  EFI_HANDLE *Drivers,
  EFI_DEVICE_PATH_PROTOCOL *Path,
  BOOLEAN Recursive
  )
{
  assert_ptr_equal (Handle, (EFI_HANDLE)(UINTN)1);
  assert_null (Drivers);
  assert_ptr_equal (Path, &mSerialConsole.TermType.Header);
  assert_false (Recursive);
  Trace ('S');
  return mConnectStatus;
}
STATIC EFI_STATUS EFIAPI
Crc (
  VOID *Data,
  UINTN Size,
  UINT32 *Value
  )
{
  *Value = 0;
  return EFI_SUCCESS;
}
UINTN EFIAPI
Print (
  CONST CHAR16 *Format,
  ...
  )
{
  mWarnings++;
  Trace ('W');
  return 0;
}

/* Stop at the actual first outside-trust-set dispatch, not at a mock gate. */
EFI_STATUS EFIAPI
EfiBootManagerDispatchDeferredImages (
  VOID
  )
{
  Trace ('U');
  assert_int_equal (mPpCount, mTpmAbsent ? 0 : 1);
  assert_int_equal (mProtectCount, mTpmAbsent ? 0 : 1);
  longjmp (mStop, 1);
}
EFI_STATUS EFIAPI
EfiBootManagerUpdateConsoleVariable (
  CONSOLE_TYPE Type,
  EFI_DEVICE_PATH_PROTOCOL *Add,
  EFI_DEVICE_PATH_PROTOCOL *Remove
  )
{
  abort ();
}
EFI_STATUS EFIAPI
EfiBootManagerRegisterContinueKeyOption (
  UINT32 Modifier,
  ...
  )
{
  abort ();
}
EFI_STATUS EFIAPI
EfiBootManagerGetBootManagerMenu (
  EFI_BOOT_MANAGER_LOAD_OPTION *Option
  )
{
  abort ();
}
EFI_STATUS EFIAPI
EfiBootManagerAddKeyOptionVariable (
  EFI_BOOT_MANAGER_KEY_OPTION *Added,
  UINT16 Number,
  UINT32 Modifier,
  ...
  )
{
  abort ();
}
EFI_BOOT_MANAGER_LOAD_OPTION *EFIAPI
EfiBootManagerGetLoadOptions (
  UINTN *Count,
  EFI_BOOT_MANAGER_LOAD_OPTION_TYPE Type
  )
{
  abort ();
}
EFI_STATUS EFIAPI
EfiBootManagerFreeLoadOptions (
  EFI_BOOT_MANAGER_LOAD_OPTION *Options,
  UINTN Count
  )
{
  abort ();
}
INTN EFIAPI
EfiBootManagerFindLoadOption (
  CONST EFI_BOOT_MANAGER_LOAD_OPTION *Key,
  CONST EFI_BOOT_MANAGER_LOAD_OPTION *Array,
  UINTN Count
  )
{
  abort ();
}
EFI_STATUS EFIAPI
EfiBootManagerAddLoadOptionVariable (
  EFI_BOOT_MANAGER_LOAD_OPTION *Option,
  UINTN Position
  )
{
  abort ();
}
CHAR16 *EFIAPI
ConvertDevicePathToText (
  CONST EFI_DEVICE_PATH_PROTOCOL *Path,
  BOOLEAN Display,
  BOOLEAN Shortcuts
  )
{
  abort ();
}
EFI_DEVICE_PATH_PROTOCOL *EFIAPI
DevicePathFromHandle (
  EFI_HANDLE Handle
  )
{
  abort ();
}
BOOLEAN EFIAPI
PostCodeEnabled (
  VOID
  )
{
  abort ();
}
UINT32 EFIAPI
PostCode (
  POST_CODE_KEY Value
  )
{
  abort ();
}

STATIC VOID
RunScenario (
  EFI_STATUS Hierarchy,
  EFI_STATUS Disable,
  EFI_STATUS CapStatus
  )
{
  mHierarchyStatus                = Hierarchy;
  mDisableStatus                  = Disable;
  mCapStatus                      = CapStatus;
  mPpCount                        = mProtectCount = mDisableCount = mCapCount = mWarnings = mTraceSize = 0;
  mStopped                        = FALSE;
  mTpmBootGateComplete            = FALSE;
  ZeroMem (&mBootServices, sizeof (mBootServices));
  ZeroMem (&mSystemTable, sizeof (mSystemTable));
  mBootServices.LocateProtocol    = LocateProtocol;
  mBootServices.LocateDevicePath  = LocateDevicePath;
  mBootServices.HandleProtocol    = HandleProtocol;
  mBootServices.ConnectController = ConnectController;
  mBootServices.CalculateCrc32    = Crc;
  gBS                             = &mBootServices;
  gST                             = &mSystemTable;
  if (setjmp (mStop) == 0) {
    PlatformBootManagerBeforeConsole ();
    fail ();
  }
  if (!mStopped) {
    UINTN PreviousPp      = mPpCount;
    UINTN PreviousProtect = mProtectCount;
    PlatformTpmBootGate ();
    assert_int_equal (mPpCount, PreviousPp);
    assert_int_equal (mProtectCount, PreviousProtect);
    assert_null (gST->ConIn);
    assert_null (gST->ConOut);
  }
  printf ("bootgate trace: %s\n", mTrace);
}
STATIC VOID
GateOrder (
  VOID **State
  )
{
  RunScenario (EFI_SUCCESS, EFI_SUCCESS, EFI_SUCCESS);
  assert_false (mStopped);
}
STATIC VOID
DisableFallback (
  VOID **State
  )
{
  RunScenario (EFI_DEVICE_ERROR, EFI_SUCCESS, EFI_SUCCESS);
  assert_false (mStopped);
  assert_int_equal (mDisableCount, 1);
}
STATIC VOID
CapFallback (
  VOID **State
  )
{
  RunScenario (EFI_DEVICE_ERROR, EFI_DEVICE_ERROR, EFI_SUCCESS);
  assert_false (mStopped);
  assert_int_equal (mCapCount, 8);
}
STATIC VOID
CapFailureStops (
  VOID **State
  )
{
  RunScenario (EFI_DEVICE_ERROR, EFI_DEVICE_ERROR, EFI_DEVICE_ERROR);
  assert_true (mStopped);
}
STATIC VOID
AbsentTpm (
  VOID **State
  )
{
  mTpmAbsent = TRUE;
  RunScenario (EFI_SUCCESS, EFI_SUCCESS, EFI_SUCCESS);
  assert_false (mStopped);
  assert_int_equal (mPpCount, 0);
  assert_int_equal (mProtectCount, 0);
  mTpmAbsent = FALSE;
}
STATIC VOID
MissingTrustedConsole (
  VOID **State
  )
{
  mConsoleStatus = EFI_NOT_FOUND;
  RunScenario (EFI_SUCCESS, EFI_SUCCESS, EFI_SUCCESS);
  assert_true (mStopped);
  assert_int_equal (mPpCount, 0);
  assert_int_equal (mProtectCount, 0);
  mConsoleStatus = EFI_SUCCESS;
}
STATIC VOID
AlreadyConnectedConsole (
  VOID **State
  )
{
  mConnectStatus = EFI_NOT_FOUND;
  RunScenario (EFI_SUCCESS, EFI_SUCCESS, EFI_SUCCESS);
  assert_false (mStopped);
  assert_int_equal (mProtectCount, 1);
  mConnectStatus = EFI_SUCCESS;
}

int
main (
  void
  )
{
  const struct CMUnitTest Tests[] = {
    cmocka_unit_test (GateOrder), cmocka_unit_test (DisableFallback), cmocka_unit_test (CapFallback), cmocka_unit_test (CapFailureStops), cmocka_unit_test (AbsentTpm), cmocka_unit_test (MissingTrustedConsole), cmocka_unit_test (AlreadyConnectedConsole)
  };
  return cmocka_run_group_tests (Tests, NULL, NULL);
}
