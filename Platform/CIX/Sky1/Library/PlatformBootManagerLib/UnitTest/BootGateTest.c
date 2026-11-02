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
#include <string.h>
#include <Library/SerialPortLib.h>

STATIC EFI_STATUS  mLocateStatus;
STATIC EFI_STATUS  mCapabilityStatus;
STATIC EFI_STATUS  mRawStatus;
STATIC TPML_PCR_SELECTION  mAllocation;
STATIC BOOLEAN  mEarlyCap;
STATIC BOOLEAN  mBadAllocation;
STATIC CHAR8  mSerialText[4096];
STATIC UINTN  mSerialLength;
STATIC CHAR8  mConsoleText[4096];
STATIC UINTN  mConsoleLength;

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
STATIC BOOLEAN                         mTpmAbsent;
STATIC BOOLEAN                         mNeedConfirm;
STATIC BOOLEAN                         mWrongConsolePath;
STATIC UINTN                           mPpCount;
STATIC UINTN                           mProtectCount;
STATIC UINTN                           mDisableCount;
STATIC UINTN                           mCapCount;
STATIC BOOLEAN                         mStopped;
STATIC EFI_SIMPLE_TEXT_INPUT_PROTOCOL  mInput;
STATIC EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL mOutput;
STATIC EFI_BOOT_SERVICES               mBootServices;
STATIC EFI_SYSTEM_TABLE                mSystemTable;
STATIC CHAR8                           mTrace[128];
STATIC UINTN                           mTraceSize;

#define SERIAL_HANDLE  ((EFI_HANDLE)(UINTN)1)
#define GOP_HANDLE     ((EFI_HANDLE)(UINTN)3)
#define USB_HANDLE     ((EFI_HANDLE)(UINTN)4)
STATIC NON_DISCOVERABLE_DEVICE   mUsbHost = { .Type = &gEdkiiNonDiscoverableXhciDeviceGuid };
STATIC EFI_DEVICE_PATH_PROTOCOL  mGopPath = {
  END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 }
};
STATIC CONSOLE_TYPE              mVarType[8];
STATIC EFI_DEVICE_PATH_PROTOCOL  *mVarPath[8];
STATIC UINTN                     mVarCount;
STATIC UINTN                     mEndOfDxeCount;

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
  mEndOfDxeCount++;
  // A prompt needs the USB controllers that register at EndOfDxe, so it
  // signals EndOfDxe before PP and the lock; other boots lock first.
  assert_int_equal (mPpCount, (mTpmAbsent || mEarlyCap || mNeedConfirm) ? 0 : 1);
  assert_int_equal (mProtectCount, (mTpmAbsent || mEarlyCap || mNeedConfirm) ? 0 : 1);
  if (mEarlyCap || (EFI_ERROR (mHierarchyStatus) && !mNeedConfirm)) {
    assert_int_equal (mCapCount, 8);
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
EFI_STATUS EFIAPI
Tpm2GetCapabilityPcrs (
  TPML_PCR_SELECTION *Pcrs
  )
{
  CopyMem (Pcrs, &mAllocation, sizeof (*Pcrs));
  return mRawStatus;
}

EFI_STATUS EFIAPI
Tpm2PcrExtend (
  TPMI_DH_PCR Pcr,
  TPML_DIGEST_VALUES *Digests
  )
{
  STATIC CONST UINT8 Sha256[] = {
    0x67, 0xab, 0xdd, 0x72, 0x10, 0x24, 0xf0, 0xff,
    0x4e, 0x0b, 0x3f, 0x4c, 0x2f, 0xc1, 0x3b, 0xc5,
    0xba, 0xd4, 0x2d, 0x0b, 0x78, 0x51, 0xd4, 0x56,
    0xd8, 0x8d, 0x20, 0x3d, 0x15, 0xaa, 0xa4, 0x50
  };
  assert_false (mBadAllocation);
  assert_int_equal (Pcr, mCapCount);
  assert_true (Digests->count >= 2);
  assert_int_equal (Digests->digests[0].hashAlg, TPM_ALG_SHA256);
  assert_memory_equal (&Digests->digests[0].digest, Sha256, sizeof (Sha256));
  assert_int_equal (Digests->digests[1].hashAlg, TPM_ALG_SHA384);
  mCapCount++;
  Trace ('C');
  return mCapStatus;
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
  // The protocol path cannot establish an independent cap.
  return EFI_UNSUPPORTED;
}
STATIC EFI_STATUS EFIAPI
Capability (
  EFI_TCG2_PROTOCOL *This,
  EFI_TCG2_BOOT_SERVICE_CAPABILITY *Cap
  )
{
  Cap->TPMPresentFlag = TRUE;
  return mCapabilityStatus;
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
    return mTpmAbsent ? EFI_NOT_FOUND : mLocateStatus;
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
  assert_ptr_equal (Guid, &gEfiSerialIoProtocolGuid);
  assert_ptr_equal (*Path, &mSerialConsole);
  assert_memory_equal (&mSerialConsole.TermType.Guid, &gEfiTtyTermGuid, sizeof (EFI_GUID));
  if (EFI_ERROR (mConsoleStatus)) {
    return mConsoleStatus;
  }
  *Path   = mWrongConsolePath ? &mSerialConsole.Uart.Header : &mSerialConsole.TermType.Header;
  *Handle = SERIAL_HANDLE;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI
HandleProtocol (
  EFI_HANDLE Handle,
  EFI_GUID *Guid,
  VOID **Interface
  )
{
  assert_ptr_equal (Handle, USB_HANDLE);
  assert_ptr_equal (Guid, &gEdkiiNonDiscoverableDeviceProtocolGuid);
  *Interface = &mUsbHost;
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
  assert_null (Drivers);
  if (Handle == SERIAL_HANDLE) {
    // Recursion lets ConPlatform and ConSplitter adopt the terminal child.
    assert_ptr_equal (Path, &mSerialConsole.TermType.Header);
    assert_true (Recursive);
    Trace ('S');
    return EFI_SUCCESS;
  }
  assert_null (Path);
  assert_false (Recursive);
  if (Handle == GOP_HANDLE) {
    Trace ('G');
  } else {
    assert_ptr_equal (Handle, USB_HANDLE);
    Trace ('K');
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI
LocateHandleBuffer (
  EFI_LOCATE_SEARCH_TYPE Type,
  EFI_GUID *Guid,
  VOID *Key,
  UINTN *Count,
  EFI_HANDLE **Buffer
  )
{
  assert_int_equal (Type, ByProtocol);
  assert_null (Key);
  *Buffer = malloc (sizeof (EFI_HANDLE));
  assert_non_null (*Buffer);
  if (Guid == &gEfiGraphicsOutputProtocolGuid) {
    **Buffer = GOP_HANDLE;
  } else {
    // The PCI root bridges are not connected yet: only SoC USB hosts exist,
    // and their drivers register them from EndOfDxe notifications.
    assert_ptr_equal (Guid, &gEdkiiNonDiscoverableDeviceProtocolGuid);
    assert_int_equal (mEndOfDxeCount, 1);
    **Buffer = USB_HANDLE;
  }
  *Count = 1;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI
FreeBuffer (
  VOID *Buffer
  )
{
  free (Buffer);
  return EFI_SUCCESS;
}
BOOLEAN EFIAPI
Tcg2PhysicalPresenceLibNeedUserConfirm (
  VOID
  )
{
  return mNeedConfirm;
}
UINTN EFIAPI
SerialPortWrite (
  UINT8 *Buffer,
  UINTN Size
  )
{
  assert_true (mSerialLength + Size < sizeof (mSerialText));
  CopyMem (mSerialText + mSerialLength, Buffer, Size);
  mSerialLength += Size;
  mSerialText[mSerialLength] = 0;
  return Size;
}

STATIC EFI_STATUS EFIAPI
OutputString (
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
  CHAR16 *Text
  )
{
  while (*Text != 0) {
    assert_true (mConsoleLength + 1 < sizeof (mConsoleText));
    mConsoleText[mConsoleLength++] = (CHAR8)*Text++;
  }
  mConsoleText[mConsoleLength] = 0;
  return EFI_SUCCESS;
}

UINTN EFIAPI
Print (
  CONST CHAR16 *Format,
  ...
  )
{
  abort ();
}

UINT64 EFIAPI
DivU64x32Remainder (UINT64 Dividend, UINT32 Divisor, UINT32 *Remainder)
{
  if (Remainder != NULL) {
    *Remainder = Dividend % Divisor;
  }
  return Dividend / Divisor;
}

UINTN EFIAPI
StrnLenS (CONST CHAR16 *String, UINTN MaxSize)
{
  UINTN Length;
  for (Length = 0; (Length < MaxSize) && (String[Length] != 0); Length++) {
  }
  return Length;
}

BOOLEAN EFIAPI
IsZeroBuffer (CONST VOID *Buffer, UINTN Size)
{
  CONST UINT8 *Bytes = Buffer;
  while (Size-- != 0) {
    if (*Bytes++ != 0) {
      return FALSE;
    }
  }
  return TRUE;
}

UINT16 EFIAPI
ReadUnaligned16 (CONST UINT16 *Buffer)
{
  UINT16 Value;
  CopyMem (&Value, Buffer, sizeof (Value));
  return Value;
}

UINT32 EFIAPI
ReadUnaligned32 (CONST UINT32 *Buffer)
{
  UINT32 Value;
  CopyMem (&Value, Buffer, sizeof (Value));
  return Value;
}

/* Stop at the actual first outside-trust-set dispatch, not at a mock gate. */
EFI_STATUS EFIAPI
EfiBootManagerDispatchDeferredImages (
  VOID
  )
{
  Trace ('U');
  assert_int_equal (mPpCount, (mTpmAbsent || mEarlyCap) ? 0 : 1);
  assert_int_equal (mProtectCount, (mTpmAbsent || mEarlyCap) ? 0 : 1);
  assert_int_equal (mEndOfDxeCount, 1);
  longjmp (mStop, 1);
}
EFI_STATUS EFIAPI
EfiBootManagerUpdateConsoleVariable (
  CONSOLE_TYPE Type,
  EFI_DEVICE_PATH_PROTOCOL *Add,
  EFI_DEVICE_PATH_PROTOCOL *Remove
  )
{
  assert_null (Remove);
  assert_true (mVarCount < ARRAY_SIZE (mVarType));
  mVarType[mVarCount]   = Type;
  mVarPath[mVarCount++] = Add;
  Trace ('V');
  return EFI_SUCCESS;
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
  return NULL;
}
EFI_DEVICE_PATH_PROTOCOL *EFIAPI
DevicePathFromHandle (
  EFI_HANDLE Handle
  )
{
  return (Handle == GOP_HANDLE) ? &mGopPath : NULL;
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
  mPpCount = mProtectCount = mDisableCount = mCapCount = mTraceSize = mVarCount = mEndOfDxeCount = 0;
  mStopped                        = FALSE;
  mSerialLength = mConsoleLength = 0;
  mSerialText[0] = mConsoleText[0] = 0;
  ZeroMem (&mAllocation, sizeof (mAllocation));
  mAllocation.count = 2;
  mAllocation.pcrSelections[0].hash = TPM_ALG_SHA256;
  mAllocation.pcrSelections[1].hash = TPM_ALG_SHA384;
  mAllocation.pcrSelections[0].sizeofSelect = 3;
  mAllocation.pcrSelections[1].sizeofSelect = 3;
  mAllocation.pcrSelections[0].pcrSelect[0] = 0xff;
  mAllocation.pcrSelections[1].pcrSelect[0] = 0xff;
  if (mBadAllocation) {
    mAllocation.pcrSelections[1].pcrSelect[0] = 0x7f;
  }
  mOutput.OutputString = OutputString;
  mTpmBootGateComplete            = FALSE;
  mEndOfDxeSignaled               = FALSE;
  ZeroMem (&mBootServices, sizeof (mBootServices));
  ZeroMem (&mSystemTable, sizeof (mSystemTable));
  // The console splitter, which owns every console that joins it.
  mSystemTable.ConIn  = &mInput;
  mSystemTable.ConOut = &mOutput;
  mBootServices.LocateProtocol     = LocateProtocol;
  mBootServices.LocateDevicePath   = LocateDevicePath;
  mBootServices.HandleProtocol     = HandleProtocol;
  mBootServices.ConnectController  = ConnectController;
  mBootServices.LocateHandleBuffer = LocateHandleBuffer;
  mBootServices.FreePool           = FreeBuffer;
  gBS                              = &mBootServices;
  gST                              = &mSystemTable;
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
    assert_ptr_equal (gST->ConIn, &mInput);
    assert_ptr_equal (gST->ConOut, &mOutput);
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
  // Without a pending request no console is connected before the lock.
  assert_string_equal (mTrace, "PHEU");
  assert_string_equal (mSerialText, "TPM BootGate: platform hierarchy protected (Success)\r\n");
  assert_string_equal (mConsoleText, mSerialText);
}
STATIC VOID
DisableFallback (
  VOID **State
  )
{
  RunScenario (EFI_DEVICE_ERROR, EFI_SUCCESS, EFI_SUCCESS);
  assert_false (mStopped);
  assert_int_equal (mDisableCount, 1);
  assert_string_equal (mSerialText,
    "TPM BootGate: hierarchy protection failed, trying disable (Device Error)\r\n"
    "TPM BootGate: platform hierarchy disabled (Success)\r\n"
    "TPM BootGate: PCRs 0-7 capped, continuing (Success)\r\n");
  assert_int_equal (mCapCount, 8);
}
STATIC VOID
CapFallback (
  VOID **State
  )
{
  RunScenario (EFI_DEVICE_ERROR, EFI_DEVICE_ERROR, EFI_SUCCESS);
  assert_false (mStopped);
  assert_int_equal (mCapCount, 8);
  assert_string_equal (mSerialText,
    "TPM BootGate: hierarchy protection failed, trying disable (Device Error)\r\n"
    "TPM BootGate: hierarchy disable failed, attempting PCR cap (Device Error)\r\n"
    "TPM BootGate: PCRs 0-7 capped, continuing (Success)\r\n");
}
STATIC VOID
CapFailureStops (
  VOID **State
  )
{
  RunScenario (EFI_DEVICE_ERROR, EFI_DEVICE_ERROR, EFI_DEVICE_ERROR);
  assert_true (mStopped);
  assert_non_null (strstr (mSerialText, "TPM BootGate: PCR cap failed, boot stopped (Device Error)\r\n"));
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
  assert_string_equal (mSerialText, "TPM BootGate: TPM absent, continuing (Not Found)\r\n");
  mTpmAbsent = FALSE;
}
STATIC BOOLEAN
Registered (
  CONSOLE_TYPE Type,
  VOID *Path
  )
{
  for (UINTN Index = 0; Index < mVarCount; Index++) {
    if ((mVarType[Index] == Type) && (mVarPath[Index] == Path)) {
      return TRUE;
    }
  }
  return FALSE;
}
STATIC VOID
PromptOnAllConsoles (
  VOID **State
  )
{
  mNeedConfirm = TRUE;
  RunScenario (EFI_SUCCESS, EFI_SUCCESS, EFI_SUCCESS);
  assert_false (mStopped);
  // EndOfDxe registers the USB controllers; ConPlatform admits only
  // registered paths; serial, GOP and USB then join before PP and the lock,
  // and the deferred-image dispatch still follows the lock.
  assert_string_equal (mTrace, "EVVVVVVSGKPHU");
  assert_true (Registered (ConIn, &mUsbKeyboard));
  assert_true (Registered (ConIn, &mSerialConsole));
  assert_true (Registered (ConOut, &mSerialConsole));
  assert_true (Registered (ConOut, &mGopPath));
  mNeedConfirm = FALSE;
}
STATIC VOID
PromptWithoutSerialTerminal (
  VOID **State
  )
{
  mNeedConfirm   = TRUE;
  mConsoleStatus = EFI_NOT_FOUND;
  RunScenario (EFI_SUCCESS, EFI_SUCCESS, EFI_SUCCESS);
  assert_false (mStopped);
  assert_string_equal (mTrace, "EVVVVVVGKPHU");
  mConsoleStatus    = EFI_SUCCESS;
  // A partial match must not start TerminalDxe on another serial device.
  mWrongConsolePath = TRUE;
  RunScenario (EFI_SUCCESS, EFI_SUCCESS, EFI_SUCCESS);
  assert_false (mStopped);
  assert_string_equal (mTrace, "EVVVVVVGKPHU");
  mWrongConsolePath = FALSE;
  mNeedConfirm      = FALSE;
}

STATIC VOID
LookupFailureCaps (VOID **State)
{
  mEarlyCap = TRUE;
  mLocateStatus = EFI_DEVICE_ERROR;
  RunScenario (EFI_SUCCESS, EFI_SUCCESS, EFI_SUCCESS);
  assert_false (mStopped);
  assert_string_equal (mTrace, "CCCCCCCCEU");
  assert_string_equal (mSerialText,
    "TPM BootGate: Tcg2 lookup failed, attempting PCR cap (Device Error)\r\n"
    "TPM BootGate: PCRs 0-7 capped, continuing (Success)\r\n");
  mLocateStatus = EFI_SUCCESS;
  mEarlyCap = FALSE;
}

STATIC VOID
CapabilityFailureCaps (VOID **State)
{
  mEarlyCap = TRUE;
  mCapabilityStatus = EFI_DEVICE_ERROR;
  RunScenario (EFI_SUCCESS, EFI_SUCCESS, EFI_SUCCESS);
  assert_false (mStopped);
  assert_string_equal (mTrace, "CCCCCCCCEU");
  assert_string_equal (mSerialText,
    "TPM BootGate: Tcg2 capability failed, attempting PCR cap (Device Error)\r\n"
    "TPM BootGate: PCRs 0-7 capped, continuing (Success)\r\n");
  mCapabilityStatus = EFI_SUCCESS;
  mEarlyCap = FALSE;
}

STATIC VOID
UnprovenCapStops (VOID **State)
{
  mEarlyCap = TRUE;
  mLocateStatus = EFI_DEVICE_ERROR;
  mRawStatus = EFI_TIMEOUT;
  RunScenario (EFI_SUCCESS, EFI_SUCCESS, EFI_SUCCESS);
  assert_true (mStopped);
  assert_int_equal (mCapCount, 0);
  assert_non_null (strstr (mSerialText, "TPM BootGate: PCR cap failed, boot stopped (Time out)\r\n"));
  mRawStatus = EFI_SUCCESS;
  mBadAllocation = TRUE;
  RunScenario (EFI_SUCCESS, EFI_SUCCESS, EFI_SUCCESS);
  assert_true (mStopped);
  assert_int_equal (mCapCount, 0);
  assert_non_null (strstr (mSerialText, "TPM BootGate: PCR cap failed, boot stopped (Compromised Data)\r\n"));
  mBadAllocation = FALSE;
  mLocateStatus = EFI_SUCCESS;
  mEarlyCap = FALSE;
}

STATIC VOID
RawCapBanks (VOID **State)
{
  STATIC CONST TPMI_ALG_HASH Algorithms[] = {
    TPM_ALG_SHA1, TPM_ALG_SHA256, TPM_ALG_SHA384, TPM_ALG_SHA512, TPM_ALG_SM3_256
  };
  UINTN Index;

  ZeroMem (&mAllocation, sizeof (mAllocation));
  mAllocation.count = ARRAY_SIZE (Algorithms);
  for (Index = 0; Index < ARRAY_SIZE (Algorithms); Index++) {
    mAllocation.pcrSelections[Index].hash = Algorithms[Index];
    mAllocation.pcrSelections[Index].sizeofSelect = 3;
    mAllocation.pcrSelections[Index].pcrSelect[0] = 0xff;
  }
  // Keep the mock's first two banks stable; cover the remaining algorithms.
  mAllocation.pcrSelections[0].hash = TPM_ALG_SHA256;
  mAllocation.pcrSelections[1].hash = TPM_ALG_SHA384;
  mAllocation.pcrSelections[2].hash = TPM_ALG_SHA1;
  mCapCount = 0;
  mCapStatus = EFI_SUCCESS;
  assert_int_equal (PlatformTpmCapPcrs (), EFI_SUCCESS);
  assert_int_equal (mCapCount, 8);
  mAllocation.pcrSelections[4].pcrSelect[0] = 0;
  mCapCount = 0;
  assert_int_equal (PlatformTpmCapPcrs (), EFI_SUCCESS);
  assert_int_equal (mCapCount, 8);
  mAllocation.pcrSelections[4].hash = 0xffff;
  mCapCount = 0;
  assert_int_equal (PlatformTpmCapPcrs (), EFI_SUCCESS);
  mAllocation.pcrSelections[4].pcrSelect[0] = 0xff;
  mCapCount = 0;
  assert_int_equal (PlatformTpmCapPcrs (), EFI_COMPROMISED_DATA);
  assert_int_equal (mCapCount, 0);
  mAllocation.pcrSelections[4].hash = TPM_ALG_SHA256;
  assert_int_equal (PlatformTpmCapPcrs (), EFI_COMPROMISED_DATA);
  mAllocation.pcrSelections[4].hash = TPM_ALG_SM3_256;
  mAllocation.pcrSelections[4].sizeofSelect = 0;
  assert_int_equal (PlatformTpmCapPcrs (), EFI_COMPROMISED_DATA);
}

int
main (
  void
  )
{
  const struct CMUnitTest Tests[] = {
    cmocka_unit_test (GateOrder), cmocka_unit_test (DisableFallback), cmocka_unit_test (CapFallback), cmocka_unit_test (CapFailureStops), cmocka_unit_test (AbsentTpm), cmocka_unit_test (PromptOnAllConsoles), cmocka_unit_test (PromptWithoutSerialTerminal)
    , cmocka_unit_test (LookupFailureCaps), cmocka_unit_test (CapabilityFailureCaps), cmocka_unit_test (UnprovenCapStops), cmocka_unit_test (RawCapBanks)
  };
  if (getenv ("GATE_TEST_FILTER") != NULL) {
    cmocka_set_test_filter (getenv ("GATE_TEST_FILTER"));
  }
  return cmocka_run_group_tests (Tests, NULL, NULL);
}
