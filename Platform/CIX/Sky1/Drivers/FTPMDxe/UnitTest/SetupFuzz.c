/** @file
  Exercise the real setup reader against arbitrary variable metadata and bytes.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <Uefi.h>
#include <PlatformSetupVar.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/Tpm2DeviceLib.h>
#include <Library/DTpmDeviceLib.h>
#include <Library/FTpmDeviceLib.h>

#define SETUP_ATTRIBUTES  (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS)

EFI_GUID                     gPlatformSetupVariableGuid = CIX_PLATFORM_SETUP_VARIABLE_GUID;
BOOLEAN                      mSetupDtpSupported;
BOOLEAN                      mSetupFtpSupported;
UINT8                        mSetupDefault;
STATIC EFI_RUNTIME_SERVICES  mRuntime;
EFI_RUNTIME_SERVICES         *gRT = &mRuntime;
STATIC PLATFORM_SETUP_DATA   mSetup;
STATIC UINTN                 mSize;
STATIC UINT32                mAttributes;
STATIC EFI_STATUS            mStatus;
STATIC UINT8                 mExpectedSelection;
STATIC UINTN                 mReads;

VOID
SetupResetSelection (
  VOID
  );

EFI_STATUS EFIAPI
Tpm2DeviceLibConstructor (
  VOID
  );

STATIC VOID
Require (
  BOOLEAN  Condition
  )
{
  if (!Condition) {
    abort ();
  }
}

STATIC EFI_STATUS EFIAPI
ReadVariable (
  CHAR16    *Name,
  EFI_GUID  *Guid,
  UINT32    *Attributes,
  UINTN     *Size,
  VOID      *Data
  )
{
  Require (mReads++ == 0);
  Require (memcmp (Name, PLATFORM_SETUP_VAR, sizeof (PLATFORM_SETUP_VAR)) == 0);
  Require (memcmp (Guid, &gPlatformSetupVariableGuid, sizeof (*Guid)) == 0);
  Require (*Size == sizeof (mSetup));
  for (UINTN Index = 0; Index < *Size; Index++) {
    Require (((UINT8 *)Data)[Index] == 0);
  }

  memcpy (Data, &mSetup, mSize < *Size ? mSize : *Size);
  *Size = mSize;
  Require (Attributes != NULL);
  *Attributes = mAttributes;
  return mStatus;
}

EFI_STATUS EFIAPI
DTpmRequestUseTpm (
  VOID
  )
{
  Require (mExpectedSelection == 1);
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
FTpmRequestUseTpm (
  VOID
  )
{
  Require (mExpectedSelection == 2);
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
DTpmSubmitCommand (
  UINT32  Size,
  UINT8   *Input,
  UINT32  *Capacity,
  UINT8   *Output
  )
{
  abort ();
}

EFI_STATUS EFIAPI
FTpmSubmitCommand (
  UINT32  Size,
  UINT8   *Input,
  UINT32  *Capacity,
  UINT8   *Output
  )
{
  abort ();
}

EFI_STATUS EFIAPI
DTpmRegisterTpm2DeviceLib (
  VOID
  )
{
  abort ();
}

EFI_STATUS EFIAPI
FTpmRegisterTpm2DeviceLib (
  VOID
  )
{
  abort ();
}

int
LLVMFuzzerTestOneInput (
  const uint8_t  *Data,
  size_t         Size
  )
{
  if (Size < 16) {
    return 0;
  }

  SetupResetSelection ();
  memset (&mSetup, 0, sizeof (mSetup));
  memset (&mRuntime, 0, sizeof (mRuntime));
  mRuntime.GetVariable = ReadVariable;
  UINT64  Reported;
  memcpy (&Reported, Data, sizeof (Reported));
  mSize = (UINTN)Reported;
  memcpy (&mAttributes, Data + 8, sizeof (mAttributes));
  switch (Data[12] % 4) {
    case 0: mStatus = EFI_SUCCESS;
      break;
    case 1: mStatus = EFI_NOT_FOUND;
      break;
    case 2: mStatus = EFI_BUFFER_TOO_SMALL;
      break;
    default: mStatus = EFI_DEVICE_ERROR;
      break;
  }

  mSetupDtpSupported = (Data[13] & 1) != 0;
  mSetupFtpSupported = (Data[13] & 2) != 0;
  mSetupDefault      = Data[14] % 3;
  if (Size > 16) {
    memcpy (&mSetup, Data + 16, Size - 16 < sizeof (mSetup) ? Size - 16 : sizeof (mSetup));
  }

  mSetup.TPMDeviceSelect = Data[15];
  // Reach all exact-schema and near-boundary cases without a giant corpus.
  if ((Data[13] & 4) != 0) {
    mSize = sizeof (mSetup);
  }

  if ((Data[13] & 8) != 0) {
    mAttributes = SETUP_ATTRIBUTES;
  }

  if ((Data[13] & 16) != 0) {
    mSize = Data[0] % (sizeof (mSetup) + 2);
  }

  if (!mSetupDtpSupported) {
    mExpectedSelection = mSetupFtpSupported ? 2 : 0;
  } else if (!mSetupFtpSupported) {
    mExpectedSelection = 1;
  } else {
    mExpectedSelection = mStatus == EFI_SUCCESS && mSize == sizeof (mSetup) &&
                         mAttributes == SETUP_ATTRIBUTES ? mSetup.TPMDeviceSelect : mSetupDefault;
  }

  mReads = 0;
  Require (Tpm2DeviceLibConstructor () == EFI_SUCCESS);
  EFI_STATUS  Status = Tpm2RequestUseTpm ();
  Require (Status == ((mExpectedSelection == 1 || mExpectedSelection == 2) ? EFI_SUCCESS : EFI_UNSUPPORTED));
  Require (mReads == ((mSetupDtpSupported && mSetupFtpSupported) ? 1 : 0));
  return 0;
}
