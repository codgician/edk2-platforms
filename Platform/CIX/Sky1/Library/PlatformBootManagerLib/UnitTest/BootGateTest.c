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
STATIC UINT8  mReplyFault;
STATIC UINT8  mMoreData;

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
STATIC BOOLEAN mSkipPp;
STATIC BOOLEAN mWrongConsolePath;
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
  assert_int_equal (mPpCount, (mTpmAbsent || mSkipPp || mEarlyCap) ? 0 : 1);
  assert_int_equal (mProtectCount, (mTpmAbsent || mEarlyCap) ? 0 : 1);
  if (mEarlyCap || EFI_ERROR (mHierarchyStatus)) {
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
  assert_int_equal (mPpCount, mSkipPp ? 0 : 1);
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
Tpm2GetCapability (
  TPM_CAP Capability,
  UINT32 Property,
  UINT32 PropertyCount,
  TPMI_YES_NO *MoreData,
  TPMS_CAPABILITY_DATA *Data
  )
{
  UINT8 *Buffer;
  UINTN Index;
  assert_int_equal (Capability, TPM_CAP_PCRS);
  assert_int_equal (Property, 0);
  assert_int_equal (PropertyCount, 1);
  ZeroMem (Data, sizeof (*Data));
  *MoreData = mMoreData;
  Data->capability = SwapBytes32 (TPM_CAP_PCRS);
  Data->data.assignedPCR.count = SwapBytes32 (mAllocation.count);
  Buffer = (UINT8 *)Data->data.assignedPCR.pcrSelections;
  for (Index = 0; Index < MIN (mAllocation.count, ARRAY_SIZE (mAllocation.pcrSelections)); Index++) {
    UINT16 Hash = SwapBytes16 (mAllocation.pcrSelections[Index].hash);
    CopyMem (Buffer, &Hash, sizeof (Hash));
    Buffer[2] = mAllocation.pcrSelections[Index].sizeofSelect;
    CopyMem (Buffer + 3, mAllocation.pcrSelections[Index].pcrSelect,
             MIN (mAllocation.pcrSelections[Index].sizeofSelect, PCR_SELECT_MAX));
    Buffer += 3 + MIN (mAllocation.pcrSelections[Index].sizeofSelect, PCR_SELECT_MAX);
  }
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

EFI_STATUS EFIAPI
Tpm2SubmitCommand (UINT32 Size, UINT8 *Input, UINT32 *OutputSize, UINT8 *Output)
{
  TPML_DIGEST_VALUES Digests;
  UINTN Index;
  UINTN Match;
  UINTN Offset = 31;
  EFI_STATUS Status;
  assert_int_equal (SwapBytes16 (ReadUnaligned16 ((UINT16 *)Input)), TPM_ST_SESSIONS);
  assert_int_equal (SwapBytes32 (ReadUnaligned32 ((UINT32 *)(Input + 2))), Size);
  assert_int_equal (SwapBytes32 (ReadUnaligned32 ((UINT32 *)(Input + 6))), TPM_CC_PCR_Extend);
  assert_int_equal (SwapBytes32 (ReadUnaligned32 ((UINT32 *)(Input + 14))), 9);
  assert_int_equal (SwapBytes32 (ReadUnaligned32 ((UINT32 *)(Input + 18))), TPM_RS_PW);
  assert_true (IsZeroBuffer (Input + 22, 5));
  ZeroMem (&Digests, sizeof (Digests));
  Digests.count = SwapBytes32 (ReadUnaligned32 ((UINT32 *)(Input + 27)));
  assert_true (Digests.count <= HASH_COUNT);
  for (Index = 0; Index < Digests.count; Index++) {
    assert_true (Offset + 2 <= Size);
    Digests.digests[Index].hashAlg = SwapBytes16 (ReadUnaligned16 ((UINT16 *)(Input + Offset)));
    Offset += 2;
    for (Match = 0; Match < ARRAY_SIZE (mTpmFailureDigests); Match++) {
      if (mTpmFailureDigests[Match].Algorithm == Digests.digests[Index].hashAlg) {
        break;
      }
    }
    assert_true (Match < ARRAY_SIZE (mTpmFailureDigests));
    assert_true (Offset + mTpmFailureDigests[Match].Size <= Size);
    assert_memory_equal (Input + Offset, mTpmFailureDigests[Match].Digest, mTpmFailureDigests[Match].Size);
    CopyMem (&Digests.digests[Index].digest, Input + Offset, mTpmFailureDigests[Match].Size);
    Offset += mTpmFailureDigests[Match].Size;
  }
  assert_int_equal (Offset, Size);
  Status = Tpm2PcrExtend (SwapBytes32 (ReadUnaligned32 ((UINT32 *)(Input + 10))), &Digests);
  assert_true (*OutputSize >= 19);
  ZeroMem (Output, 19);
  Output[0] = 0x80;
  Output[1] = 0x02;
  Output[5] = 19;
  *OutputSize = 19;
  switch (mReplyFault) {
    case 1: *OutputSize = 10; Output[1] = 1; Output[5] = 10; break;
    case 2: Output[5] = 18; break;
    case 3: Output[9] = 1; break;
    case 4: Output[13] = 1; break;
    case 5: Output[15] = 1; break;
    case 6: Output[16] = 2; break;
    case 7: Output[18] = 1; break;
  }
  return Status;
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
  if (mWrongConsolePath) {
    *Path = (EFI_DEVICE_PATH_PROTOCOL *)&mSerialConsole;
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

UINT16 EFIAPI SwapBytes16 (UINT16 Value) { return __builtin_bswap16 (Value); }
UINT32 EFIAPI SwapBytes32 (UINT32 Value) { return __builtin_bswap32 (Value); }
UINT16 EFIAPI WriteUnaligned16 (UINT16 *Buffer, UINT16 Value) {
  volatile UINT8 *Bytes = (volatile UINT8 *)Buffer;
  Bytes[0] = (UINT8)Value;
  Bytes[1] = (UINT8)(Value >> 8);
  return Value;
}
UINT32 EFIAPI WriteUnaligned32 (UINT32 *Buffer, UINT32 Value) {
  volatile UINT8 *Bytes = (volatile UINT8 *)Buffer;
  Bytes[0] = (UINT8)Value;
  Bytes[1] = (UINT8)(Value >> 8);
  Bytes[2] = (UINT8)(Value >> 16);
  Bytes[3] = (UINT8)(Value >> 24);
  return Value;
}

/* Stop at the actual first outside-trust-set dispatch, not at a mock gate. */
EFI_STATUS EFIAPI
EfiBootManagerDispatchDeferredImages (
  VOID
  )
{
  Trace ('U');
  assert_int_equal (mPpCount, (mTpmAbsent || mSkipPp || mEarlyCap) ? 0 : 1);
  assert_int_equal (mProtectCount, (mTpmAbsent || mEarlyCap) ? 0 : 1);
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
  mPpCount = mProtectCount = mDisableCount = mCapCount = mTraceSize = 0;
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
STATIC VOID
MissingTrustedConsole (
  VOID **State
  )
{
  mConsoleStatus = EFI_NOT_FOUND;
  mSkipPp = TRUE;
  RunScenario (EFI_SUCCESS, EFI_SUCCESS, EFI_SUCCESS);
  assert_false (mStopped);
  assert_int_equal (mPpCount, 0);
  assert_int_equal (mProtectCount, 1);
  assert_non_null (strstr (mSerialText, "TPM BootGate: PP skipped (no trusted console) (Not Found)\r\n"));
  assert_string_equal (mTrace, "HEU");
  mConsoleStatus = EFI_SUCCESS;
  mSkipPp = FALSE;
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

STATIC VOID
UnresolvableTrustedConsole (
  VOID **State
  )
{
  mSkipPp = TRUE;
  mWrongConsolePath = TRUE;
  RunScenario (EFI_SUCCESS, EFI_SUCCESS, EFI_SUCCESS);
  assert_false (mStopped);
  assert_int_equal (mPpCount, 0);
  assert_int_equal (mProtectCount, 1);
  mWrongConsolePath = FALSE;
  mSkipPp = FALSE;
}

STATIC VOID
MissingConsoleFallbacks (
  VOID **State
  )
{
  mConsoleStatus = EFI_NOT_FOUND;
  mSkipPp = TRUE;
  RunScenario (EFI_DEVICE_ERROR, EFI_SUCCESS, EFI_SUCCESS);
  assert_false (mStopped);
  assert_int_equal (mDisableCount, 1);
  assert_int_equal (mPpCount, 0);
  RunScenario (EFI_DEVICE_ERROR, EFI_DEVICE_ERROR, EFI_SUCCESS);
  assert_false (mStopped);
  assert_int_equal (mCapCount, 8);
  assert_int_equal (mPpCount, 0);
  RunScenario (EFI_DEVICE_ERROR, EFI_DEVICE_ERROR, EFI_DEVICE_ERROR);
  assert_true (mStopped);
  assert_int_equal (mPpCount, 0);
  mConsoleStatus = EFI_SUCCESS;
  mSkipPp = FALSE;
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
  mAllocation.pcrSelections[4].sizeofSelect = PCR_SELECT_MAX + 1;
  assert_int_equal (PlatformTpmCapPcrs (), EFI_COMPROMISED_DATA);
  mAllocation.count = ARRAY_SIZE (mAllocation.pcrSelections) + 1;
  assert_int_equal (PlatformTpmCapPcrs (), EFI_COMPROMISED_DATA);

  mAllocation.count = 2;
  mMoreData = 1;
  assert_int_equal (PlatformTpmCapPcrs (), EFI_COMPROMISED_DATA);
  mMoreData = 0;
  for (mReplyFault = 1; mReplyFault <= 7; mReplyFault++) {
    mCapCount = 0;
    assert_int_equal (PlatformTpmCapPcrs (), EFI_DEVICE_ERROR);
    assert_int_equal (mCapCount, 1);
  }
  mReplyFault = 0;
}

int
main (
  void
  )
{
  const struct CMUnitTest Tests[] = {
    cmocka_unit_test (GateOrder), cmocka_unit_test (DisableFallback), cmocka_unit_test (CapFallback), cmocka_unit_test (CapFailureStops), cmocka_unit_test (AbsentTpm), cmocka_unit_test (MissingTrustedConsole), cmocka_unit_test (AlreadyConnectedConsole), cmocka_unit_test (UnresolvableTrustedConsole), cmocka_unit_test (MissingConsoleFallbacks)
    , cmocka_unit_test (LookupFailureCaps), cmocka_unit_test (CapabilityFailureCaps), cmocka_unit_test (UnprovenCapStops), cmocka_unit_test (RawCapBanks)
  };
  if (getenv ("GATE_TEST_FILTER") != NULL) {
    cmocka_set_test_filter (getenv ("GATE_TEST_FILTER"));
  }
  return cmocka_run_group_tests (Tests, NULL, NULL);
}
