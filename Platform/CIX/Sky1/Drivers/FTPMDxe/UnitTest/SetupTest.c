/** @file
  Exact setup-variable schema and supported-device routing regression tests.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <cmocka.h>

#include <Uefi.h>
#include <PlatformSetupVar.h>
#include <Library/Tpm2DeviceLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/DTpmDeviceLib.h>
#include <Library/FTpmDeviceLib.h>

#define SETUP_ATTRIBUTES  (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS)
#define DTPM_SELECTION    1
#define FTPM_SELECTION    2

EFI_GUID  gPlatformSetupVariableGuid = CIX_PLATFORM_SETUP_VARIABLE_GUID;
BOOLEAN   mSetupDtpSupported;
BOOLEAN   mSetupFtpSupported;
UINT8     mSetupDefault;

VOID
SetupResetSelection (
  VOID
  );

EFI_STATUS EFIAPI
Tpm2DeviceLibConstructor (
  VOID
  );

EFI_STATUS EFIAPI
DTpmSwitchFTpm (
  VOID
  );

STATIC EFI_RUNTIME_SERVICES  mRuntime;
EFI_RUNTIME_SERVICES         *gRT = &mRuntime;
STATIC PLATFORM_SETUP_DATA   mReadData;
STATIC UINTN                 mReadSize;
STATIC UINT32                mReadAttributes;
STATIC EFI_STATUS            mReadStatus;
STATIC EFI_STATUS            mWriteStatus;
STATIC UINTN                 mExpectedReads;
STATIC UINTN                 mExpectedWrites;
STATIC BOOLEAN               mInitiallyZero;
STATIC UINT8                 mExpectedDevice;
STATIC UINTN                 mExpectedSubmits;
STATIC UINTN                 mExpectedRegisters;
STATIC UINTN                 mExpectedDtpRequests;
STATIC UINTN                 mExpectedFtpRequests;
STATIC EFI_STATUS            mDtpRequestStatus;
STATIC EFI_STATUS            mFtpRequestStatus;

STATIC
VOID
CheckVariable (
  CHAR16    *Name,
  EFI_GUID  *Guid
  )
{
  assert_memory_equal (Name, PLATFORM_SETUP_VAR, sizeof (PLATFORM_SETUP_VAR));
  assert_memory_equal (Guid, &gPlatformSetupVariableGuid, sizeof (*Guid));
}

STATIC
EFI_STATUS
EFIAPI
GetVariable (
  IN CHAR16     *Name,
  IN EFI_GUID   *Guid,
  OUT UINT32    *Attributes OPTIONAL,
  IN OUT UINTN  *Size,
  OUT VOID      *Data OPTIONAL
  )
{
  UINTN  Index;
  UINTN  CopySize;

  assert_true (mExpectedReads != 0);
  mExpectedReads--;
  CheckVariable (Name, Guid);
  assert_int_equal (*Size, sizeof (mReadData));
  assert_non_null (Data);
  for (Index = 0; Index < *Size; Index++) {
    if (((UINT8 *)Data)[Index] != 0) {
      mInitiallyZero = FALSE;
    }
  }

  // Model even hostile services without overwriting the caller's capacity.
  CopySize = mReadSize < *Size ? mReadSize : *Size;
  memcpy (Data, &mReadData, CopySize);
  *Size = mReadSize;
  if (Attributes != NULL) {
    *Attributes = mReadAttributes;
  }

  return mReadStatus;
}

STATIC
EFI_STATUS
EFIAPI
SetVariable (
  IN CHAR16    *Name,
  IN EFI_GUID  *Guid,
  IN UINT32    Attributes,
  IN UINTN     Size,
  IN VOID      *Data
  )
{
  PLATFORM_SETUP_DATA  Expected;

  assert_true (mExpectedWrites != 0);
  mExpectedWrites--;
  CheckVariable (Name, Guid);
  assert_int_equal (Attributes, SETUP_ATTRIBUTES);
  assert_int_equal (Size, sizeof (Expected));
  Expected                 = mReadData;
  Expected.TPMDeviceSelect = FTPM_SELECTION;
  // Every unrelated persisted field must come from the complete valid read.
  assert_memory_equal (Data, &Expected, sizeof (Expected));
  return mWriteStatus;
}

STATIC
VOID
Reset (
  VOID
  )
{
  SetupResetSelection ();
  memset (&mRuntime, 0, sizeof (mRuntime));
  mRuntime.GetVariable = GetVariable;
  mRuntime.SetVariable = SetVariable;
  memset (&mReadData, 0xA5, sizeof (mReadData));
  mReadData.TPMDeviceSelect = DTPM_SELECTION;
  mReadSize                 = sizeof (mReadData);
  mReadAttributes           = SETUP_ATTRIBUTES;
  mReadStatus               = EFI_SUCCESS;
  mWriteStatus              = EFI_SUCCESS;
  mExpectedReads            = 1;
  mExpectedWrites           = 0;
  mInitiallyZero            = TRUE;
  mSetupDtpSupported        = TRUE;
  mSetupFtpSupported        = TRUE;
  mSetupDefault             = FTPM_SELECTION;
  mExpectedDevice           = 0;
  mExpectedSubmits          = 0;
  mExpectedRegisters        = 0;
  mExpectedDtpRequests      = 0;
  mExpectedFtpRequests      = 0;
  mDtpRequestStatus         = EFI_SUCCESS;
  mFtpRequestStatus         = EFI_SUCCESS;
}

STATIC
VOID
CheckConsumed (
  VOID
  )
{
  assert_int_equal (mExpectedReads, 0);
  assert_int_equal (mExpectedWrites, 0);
  assert_int_equal (mExpectedSubmits, 0);
  assert_int_equal (mExpectedRegisters, 0);
  assert_int_equal (mExpectedDtpRequests, 0);
  assert_int_equal (mExpectedFtpRequests, 0);
}

STATIC
EFI_STATUS
Submit (
  UINT8   Device,
  UINT32  InputSize,
  UINT8   *Input,
  UINT32  *OutputSize,
  UINT8   *Output
  )
{
  assert_true (mExpectedSubmits != 0);
  mExpectedSubmits--;
  assert_int_equal (Device, mExpectedDevice);
  assert_int_equal (InputSize, 1);
  assert_int_equal (*OutputSize, 1);
  assert_int_equal (*Input, 0x12);
  *Output = Device;
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
DTpmSubmitCommand (
  UINT32  InSize,
  UINT8   *In,
  UINT32  *OutSize,
  UINT8   *Out
  )
{
  return Submit (DTPM_SELECTION, InSize, In, OutSize, Out);
}

EFI_STATUS EFIAPI
FTpmSubmitCommand (
  UINT32  InSize,
  UINT8   *In,
  UINT32  *OutSize,
  UINT8   *Out
  )
{
  return Submit (FTPM_SELECTION, InSize, In, OutSize, Out);
}

EFI_STATUS EFIAPI
DTpmRegisterTpm2DeviceLib (
  VOID
  )
{
  assert_true (mExpectedRegisters != 0);
  mExpectedRegisters--;
  assert_int_equal (mExpectedDevice, DTPM_SELECTION);
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
FTpmRegisterTpm2DeviceLib (
  VOID
  )
{
  assert_true (mExpectedRegisters != 0);
  mExpectedRegisters--;
  assert_int_equal (mExpectedDevice, FTPM_SELECTION);
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
DTpmRequestUseTpm (
  VOID
  )
{
  assert_true (mExpectedDtpRequests != 0);
  mExpectedDtpRequests--;
  return mDtpRequestStatus;
}

EFI_STATUS EFIAPI
FTpmRequestUseTpm (
  VOID
  )
{
  assert_true (mExpectedFtpRequests != 0);
  mExpectedFtpRequests--;
  return mFtpRequestStatus;
}

STATIC
VOID
CheckRoute (
  UINT8  Device
  )
{
  UINT8   Input      = 0x12;
  UINT8   Output     = 0;
  UINT32  OutputSize = 1;

  mExpectedDevice = Device;
  if ((Device == DTPM_SELECTION) || (Device == FTPM_SELECTION)) {
    mExpectedSubmits   = 1;
    mExpectedRegisters = 1;
    assert_int_equal (Tpm2SubmitCommand (1, &Input, &OutputSize, &Output), EFI_SUCCESS);
    assert_int_equal (Output, Device);
    assert_int_equal (Tpm2RegisterTpm2DeviceLib (NULL), EFI_SUCCESS);
  } else {
    assert_int_equal (Tpm2SubmitCommand (1, &Input, &OutputSize, &Output), EFI_UNSUPPORTED);
    assert_int_equal (Tpm2RegisterTpm2DeviceLib (NULL), EFI_UNSUPPORTED);
  }
}

STATIC
VOID
ConstructorSizes (
  VOID  **State
  )
{
  UINTN  Size;

  (VOID)State;
  for (Size = 0; Size <= sizeof (mReadData) + 1; Size++) {
    Reset ();
    mReadSize = Size;
    assert_int_equal (Tpm2DeviceLibConstructor (), EFI_SUCCESS);
    CheckRoute ((Size == sizeof (mReadData)) ? DTPM_SELECTION : FTPM_SELECTION);
    CheckConsumed ();
  }
}

STATIC
VOID
ConstructorAttributes (
  VOID  **State
  )
{
  UINT32  Attributes;

  (VOID)State;
  for (Attributes = 0; Attributes <= 128; Attributes++) {
    Reset ();
    mReadAttributes = (Attributes == 128) ? MAX_UINT32 : Attributes;
    assert_int_equal (Tpm2DeviceLibConstructor (), EFI_SUCCESS);
    CheckRoute ((mReadAttributes == SETUP_ATTRIBUTES) ? DTPM_SELECTION : FTPM_SELECTION);
    CheckConsumed ();
  }
}

STATIC CONST EFI_STATUS  mErrors[] = {
  EFI_NOT_FOUND,     EFI_DEVICE_ERROR,       EFI_BUFFER_TOO_SMALL,
  EFI_ACCESS_DENIED, EFI_SECURITY_VIOLATION, EFI_OUT_OF_RESOURCES
};

STATIC
VOID
ConstructorErrors (
  VOID  **State
  )
{
  UINTN  Index;

  (VOID)State;
  for (Index = 0; Index < sizeof (mErrors) / sizeof (mErrors[0]); Index++) {
    Reset ();
    mReadStatus = mErrors[Index];
    assert_int_equal (Tpm2DeviceLibConstructor (), EFI_SUCCESS);
    CheckRoute (FTPM_SELECTION);
    // The safe default is cached, rather than retrying a hostile variable.
    assert_int_equal (Tpm2DeviceLibConstructor (), EFI_SUCCESS);
    CheckConsumed ();
  }
}

STATIC
VOID
ConstructorSelections (
  VOID  **State
  )
{
  UINTN  Selection;
  UINTN  Support;
  UINT8  Expected;

  (VOID)State;
  for (Support = 0; Support < 4; Support++) {
    for (Selection = 0; Selection < 256; Selection++) {
      Reset ();
      mSetupDtpSupported        = (Support & 1) != 0;
      mSetupFtpSupported        = (Support & 2) != 0;
      mReadData.TPMDeviceSelect = (UINT8)Selection;
      Expected                  = (UINT8)Selection;
      if ((Selection == DTPM_SELECTION) && !mSetupDtpSupported && mSetupFtpSupported) {
        Expected = FTPM_SELECTION;
      } else if ((Selection == FTPM_SELECTION) && !mSetupFtpSupported && mSetupDtpSupported) {
        Expected = DTPM_SELECTION;
      }

      assert_int_equal (Tpm2DeviceLibConstructor (), EFI_SUCCESS);
      CheckRoute (Expected);
      if (Selection != 255) {
        assert_int_equal (Tpm2DeviceLibConstructor (), EFI_SUCCESS);
      }

      CheckConsumed ();
    }
  }
}

STATIC
VOID
ConstructorDefaults (
  VOID  **State
  )
{
  UINTN  Selection;
  UINTN  Support;
  UINT8  Expected;

  (VOID)State;
  for (Support = 0; Support < 4; Support++) {
    for (Selection = 0; Selection <= FTPM_SELECTION; Selection++) {
      Reset ();
      mSetupDefault      = (UINT8)Selection;
      mSetupDtpSupported = (Support & 1) != 0;
      mSetupFtpSupported = (Support & 2) != 0;
      mReadStatus        = EFI_NOT_FOUND;
      Expected           = mSetupDefault;
      if ((Expected == DTPM_SELECTION) && !mSetupDtpSupported && mSetupFtpSupported) {
        Expected = FTPM_SELECTION;
      } else if ((Expected == FTPM_SELECTION) && !mSetupFtpSupported && mSetupDtpSupported) {
        Expected = DTPM_SELECTION;
      }

      assert_int_equal (Tpm2DeviceLibConstructor (), EFI_SUCCESS);
      CheckRoute (Expected);
      CheckConsumed ();
    }
  }
}

STATIC
VOID
ZeroedReadBuffers (
  VOID  **State
  )
{
  (VOID)State;
  Reset ();
  assert_int_equal (Tpm2DeviceLibConstructor (), EFI_SUCCESS);
  assert_true (mInitiallyZero);
  mExpectedReads  = 1;
  mExpectedWrites = 1;
  mInitiallyZero  = TRUE;
  assert_int_equal (DTpmSwitchFTpm (), EFI_SUCCESS);
  assert_true (mInitiallyZero);
  CheckConsumed ();
}

STATIC
VOID
FallbackSizes (
  VOID  **State
  )
{
  UINTN  Size;

  (VOID)State;
  for (Size = 0; Size <= sizeof (mReadData) + 1; Size++) {
    Reset ();
    assert_int_equal (Tpm2DeviceLibConstructor (), EFI_SUCCESS);
    mExpectedReads  = 1;
    mReadSize       = Size;
    mExpectedWrites = (Size == sizeof (mReadData)) ? 1 : 0;
    assert_int_equal (
                      DTpmSwitchFTpm (),
                      (Size == sizeof (mReadData)) ? EFI_SUCCESS : EFI_COMPROMISED_DATA
                      );
    CheckConsumed ();
  }
}

STATIC
VOID
FallbackAttributesAndErrors (
  VOID  **State
  )
{
  UINT32  Attributes;
  UINTN   Index;

  (VOID)State;
  for (Attributes = 0; Attributes <= 128; Attributes++) {
    Reset ();
    mReadAttributes = (Attributes == 128) ? MAX_UINT32 : Attributes;
    mExpectedWrites = (mReadAttributes == SETUP_ATTRIBUTES) ? 1 : 0;
    assert_int_equal (
                      DTpmSwitchFTpm (),
                      (mReadAttributes == SETUP_ATTRIBUTES) ? EFI_SUCCESS : EFI_COMPROMISED_DATA
                      );
    CheckConsumed ();
  }

  for (Index = 0; Index < sizeof (mErrors) / sizeof (mErrors[0]); Index++) {
    Reset ();
    mReadStatus = mErrors[Index];
    assert_int_equal (DTpmSwitchFTpm (), mErrors[Index]);
    CheckConsumed ();
  }
}

STATIC
VOID
FallbackRouting (
  VOID  **State
  )
{
  UINTN  Mode;

  (VOID)State;
  for (Mode = 0; Mode < 4; Mode++) {
    Reset ();
    assert_int_equal (Tpm2DeviceLibConstructor (), EFI_SUCCESS);
    mExpectedReads       = 1;
    mExpectedDtpRequests = 1;
    mExpectedFtpRequests = 1;
    mDtpRequestStatus    = EFI_NOT_FOUND;
    if (Mode == 0) {
      mExpectedWrites = 1;
    } else if (Mode == 1) {
      mReadSize = sizeof (mReadData) - 1;
    } else if (Mode == 2) {
      mReadAttributes |= EFI_VARIABLE_RUNTIME_ACCESS;
    } else {
      mReadStatus = EFI_NOT_FOUND;
    }

    assert_int_equal (Tpm2RequestUseTpm (), EFI_SUCCESS);
    CheckConsumed ();
    if (Mode == 0) {
      CheckRoute (FTPM_SELECTION);
    }

    CheckConsumed ();
  }

  Reset ();
  assert_int_equal (Tpm2DeviceLibConstructor (), EFI_SUCCESS);
  mExpectedDtpRequests = 1;
  assert_int_equal (Tpm2RequestUseTpm (), EFI_SUCCESS);
  CheckRoute (DTPM_SELECTION);
  CheckConsumed ();

  Reset ();
  mReadData.TPMDeviceSelect = FTPM_SELECTION;
  assert_int_equal (Tpm2DeviceLibConstructor (), EFI_SUCCESS);
  mExpectedFtpRequests = 1;
  mFtpRequestStatus    = EFI_DEVICE_ERROR;
  assert_int_equal (Tpm2RequestUseTpm (), EFI_DEVICE_ERROR);
  CheckConsumed ();
}

STATIC
VOID
FallbackWriteError (
  VOID  **State
  )
{
  (VOID)State;
  Reset ();
  assert_int_equal (Tpm2DeviceLibConstructor (), EFI_SUCCESS);
  mExpectedReads  = 1;
  mExpectedWrites = 1;
  mWriteStatus    = EFI_WRITE_PROTECTED;
  assert_int_equal (DTpmSwitchFTpm (), EFI_WRITE_PROTECTED);
  CheckRoute (DTPM_SELECTION);
  CheckConsumed ();
}

int
main (
  void
  )
{
  const struct CMUnitTest  Tests[] = {
    cmocka_unit_test (ConstructorSizes),
    cmocka_unit_test (ConstructorAttributes),
    cmocka_unit_test (ConstructorErrors),
    cmocka_unit_test (ConstructorSelections),
    cmocka_unit_test (ConstructorDefaults),
    cmocka_unit_test (ZeroedReadBuffers),
    cmocka_unit_test (FallbackSizes),
    cmocka_unit_test (FallbackAttributesAndErrors),
    cmocka_unit_test (FallbackRouting),
    cmocka_unit_test (FallbackWriteError)
  };

  return cmocka_run_group_tests (Tests, NULL, NULL);
}
