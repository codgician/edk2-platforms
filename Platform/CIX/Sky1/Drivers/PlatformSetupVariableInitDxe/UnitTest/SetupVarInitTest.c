/** @file
  Host tests for the setup variable initialisers: every stored size is
  either trusted exactly, migrated from a known layout, or replaced with
  current-size defaults; nothing is written beyond or read past a buffer.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <PlatformSetupVar.h>
#include <Guid/NetworkStackSetup.h>

#define SETUP_ATTR  (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS)

EFI_STATUS
PlatformSetupVariableInit (
  VOID
  );

EFI_STATUS
NetworkStackVariableInit (
  VOID
  );

EFI_STATUS
SystemTableVariableInit (
  VOID
  );

VOID
EFIAPI
ConstructSetupVariable (
  PLATFORM_SETUP_DATA  *PlatformSetupVar
  );

EFI_GUID  gPlatformSetupVariableGuid = CIX_PLATFORM_SETUP_VARIABLE_GUID;
EFI_GUID  gEfiNetworkStackSetupGuid  = NETWORK_STACK_SETUP_VARIABLE_GUID;
EFI_GUID  gCixGlobalVariableGuid     = {
  0x31d1bbc6, 0xc793, 0x48af, { 0x8d, 0xcf, 0xf9, 0x18, 0x38, 0xc7, 0x3a, 0x2e }
};

typedef struct {
  BOOLEAN       Exists;
  UINT8         Data[512];
  UINTN         Size;
  UINT32        Attributes;
  UINTN         Writes;
  EFI_STATUS    ReadStatus;
} TEST_VARIABLE;

STATIC TEST_VARIABLE  mVariable;
STATIC CHAR16         *mName;
STATIC EFI_GUID       *mGuid;

VOID
EFIAPI
GetValidCpuCoreNum (
  IN OUT UINT8  *ValidCpuCoreNum
  )
{
  *ValidCpuCoreNum = 12;
}

VOID
EFIAPI
GetCpuShareInfo (
  IN OUT UINT32  *CpuShareInfo
  )
{
  *CpuShareInfo = 0x12345678;
}

STATIC EFI_STATUS EFIAPI
MockGetVariable (
  CHAR16    *Name,
  EFI_GUID  *Guid,
  UINT32    *Attributes,
  UINTN     *DataSize,
  VOID      *Data
  )
{
  assert_int_equal (StrCmp (Name, mName), 0);
  assert_true (CompareGuid (Guid, mGuid));
  if (EFI_ERROR (mVariable.ReadStatus)) {
    return mVariable.ReadStatus;
  }

  if (!mVariable.Exists) {
    return EFI_NOT_FOUND;
  }

  if (*DataSize < mVariable.Size) {
    *DataSize = mVariable.Size;
    return EFI_BUFFER_TOO_SMALL;
  }

  // Copy exactly the stored size so ASan sees any undersized buffer.
  CopyMem (Data, mVariable.Data, mVariable.Size);
  *DataSize = mVariable.Size;
  if (Attributes != NULL) {
    *Attributes = mVariable.Attributes;
  }

  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
MockSetVariable (
  CHAR16    *Name,
  EFI_GUID  *Guid,
  UINT32    Attributes,
  UINTN     DataSize,
  VOID      *Data
  )
{
  assert_int_equal (StrCmp (Name, mName), 0);
  assert_true (CompareGuid (Guid, mGuid));
  assert_true (DataSize <= sizeof (mVariable.Data));
  mVariable.Writes++;
  mVariable.Exists     = TRUE;
  mVariable.Attributes = Attributes;
  mVariable.Size       = DataSize;
  ZeroMem (mVariable.Data, sizeof (mVariable.Data));
  CopyMem (mVariable.Data, Data, DataSize);
  return EFI_SUCCESS;
}

STATIC EFI_RUNTIME_SERVICES  mRuntime = {
  .GetVariable = MockGetVariable,
  .SetVariable = MockSetVariable
};
STATIC EFI_BOOT_SERVICES     mBoot;
EFI_RUNTIME_SERVICES         *gRT = &mRuntime;
EFI_BOOT_SERVICES            *gBS = &mBoot;

STATIC VOID
UseVariable (
  CHAR16    *Name,
  EFI_GUID  *Guid
  )
{
  ZeroMem (&mVariable, sizeof (mVariable));
  mName = Name;
  mGuid = Guid;
}

STATIC VOID
StoreVariable (
  CONST VOID  *Data,
  UINTN       Size
  )
{
  assert_true (Size <= sizeof (mVariable.Data));
  mVariable.Exists     = TRUE;
  mVariable.Attributes = SETUP_ATTR;
  mVariable.Size       = Size;
  CopyMem (mVariable.Data, Data, Size);
}

STATIC VOID
StorePattern (
  UINTN  Size
  )
{
  UINT8  Pattern[sizeof (mVariable.Data)];
  UINTN  Index;

  // Never 0 or 1, so stored bytes cannot pass for defaults.
  for (Index = 0; Index < Size; Index++) {
    Pattern[Index] = (UINT8)(0x80 | (Index & 0x7F));
  }

  StoreVariable (Pattern, Size);
}

STATIC VOID
ExpectWritten (
  CONST VOID  *Expected,
  UINTN       Size
  )
{
  assert_int_equal (mVariable.Writes, 1);
  assert_int_equal (mVariable.Attributes, SETUP_ATTR);
  assert_int_equal (mVariable.Size, Size);
  assert_memory_equal (mVariable.Data, Expected, Size);
}

STATIC VOID
SetupDefaults (
  PLATFORM_SETUP_DATA  *Defaults
  )
{
  ZeroMem (Defaults, sizeof (*Defaults));
  ConstructSetupVariable (Defaults);
}

STATIC VOID
SetupNotFoundCreatesDefaults (
  VOID  **State
  )
{
  PLATFORM_SETUP_DATA  Defaults;

  UseVariable (PLATFORM_SETUP_VAR, &gPlatformSetupVariableGuid);
  assert_int_equal (PlatformSetupVariableInit (), EFI_SUCCESS);
  SetupDefaults (&Defaults);
  ExpectWritten (&Defaults, sizeof (Defaults));
  assert_int_equal (Defaults.TPMDeviceSelect, 1);
  assert_int_equal (Defaults.CpuShareInfo, 0x12345678);
}

STATIC VOID
SetupExactSizeIsKept (
  VOID  **State
  )
{
  UINT8  Before[sizeof (PLATFORM_SETUP_DATA)];

  UseVariable (PLATFORM_SETUP_VAR, &gPlatformSetupVariableGuid);
  StorePattern (sizeof (PLATFORM_SETUP_DATA));
  CopyMem (Before, mVariable.Data, sizeof (Before));
  assert_int_equal (PlatformSetupVariableInit (), EFI_SUCCESS);
  assert_int_equal (mVariable.Writes, 0);
  assert_memory_equal (mVariable.Data, Before, sizeof (Before));
}

STATIC VOID
SetupOversizedGetsDefaults (
  VOID  **State
  )
{
  PLATFORM_SETUP_DATA  Defaults;

  UseVariable (PLATFORM_SETUP_VAR, &gPlatformSetupVariableGuid);
  StorePattern (sizeof (mVariable.Data));
  assert_int_equal (PlatformSetupVariableInit (), EFI_SUCCESS);
  SetupDefaults (&Defaults);
  ExpectWritten (&Defaults, sizeof (Defaults));
}

STATIC VOID
SetupUnknownShortGetsDefaults (
  VOID  **State
  )
{
  PLATFORM_SETUP_DATA  Defaults;

  // Beta2/2025Q3 and Beta1 RC3 layouts share this size with different orders.
  UseVariable (PLATFORM_SETUP_VAR, &gPlatformSetupVariableGuid);
  StorePattern (224);
  assert_int_equal (PlatformSetupVariableInit (), EFI_SUCCESS);
  SetupDefaults (&Defaults);
  ExpectWritten (&Defaults, sizeof (Defaults));

  UseVariable (PLATFORM_SETUP_VAR, &gPlatformSetupVariableGuid);
  StorePattern (1);
  assert_int_equal (PlatformSetupVariableInit (), EFI_SUCCESS);
  ExpectWritten (&Defaults, sizeof (Defaults));
}

STATIC VOID
Setup2025Q4IsMigrated (
  VOID  **State
  )
{
  PLATFORM_SETUP_DATA  Current;
  PLATFORM_SETUP_DATA  Defaults;
  UINT8                Legacy[sizeof (PLATFORM_SETUP_DATA)];
  UINTN                Role;
  UINTN                Tail;
  UINTN                Index;

  //
  // A 2025Q4 variable is the current layout without Usb3Control0DataRole and
  // the trailing SmmuEnable, SPEEnable and FastBootHotKeyEnable.
  //
  for (Index = 0; Index < sizeof (Current); Index++) {
    ((UINT8 *)&Current)[Index] = (UINT8)(0x80 | (Index & 0x7F));
  }

  Role = OFFSET_OF (PLATFORM_SETUP_DATA, Usb3Control0DataRole);
  Tail = OFFSET_OF (PLATFORM_SETUP_DATA, SmmuEnable);
  assert_int_equal (Tail + 3, sizeof (Current));
  CopyMem (Legacy, &Current, Role);
  CopyMem (Legacy + Role, (UINT8 *)&Current + Role + 1, Tail - Role - 1);

  UseVariable (PLATFORM_SETUP_VAR, &gPlatformSetupVariableGuid);
  StoreVariable (Legacy, sizeof (Current) - 4);
  assert_int_equal (PlatformSetupVariableInit (), EFI_SUCCESS);

  SetupDefaults (&Defaults);
  Current.Usb3Control0DataRole = Defaults.Usb3Control0DataRole;
  Current.SmmuEnable           = Defaults.SmmuEnable;
  Current.SPEEnable            = Defaults.SPEEnable;
  Current.FastBootHotKeyEnable = Defaults.FastBootHotKeyEnable;
  Current.TPMDeviceSelect      = Defaults.TPMDeviceSelect;
  ExpectWritten (&Current, sizeof (Current));
}

STATIC VOID
SetupReadErrorIsNotOverwritten (
  VOID  **State
  )
{
  UseVariable (PLATFORM_SETUP_VAR, &gPlatformSetupVariableGuid);
  StorePattern (sizeof (PLATFORM_SETUP_DATA));
  mVariable.ReadStatus = EFI_DEVICE_ERROR;
  assert_int_equal (PlatformSetupVariableInit (), EFI_DEVICE_ERROR);
  assert_int_equal (mVariable.Writes, 0);
}

STATIC VOID
NetworkStackSizes (
  VOID  **State
  )
{
  STATIC CONST UINTN  BadSizes[] = { 0, 3, 64 };
  NETWORK_STACK       Defaults = { 1, 1, 1, 1, 1 };
  UINT8               Before[sizeof (NETWORK_STACK)];
  UINTN               Index;

  for (Index = 0; Index < ARRAY_SIZE (BadSizes); Index++) {
    UseVariable (NETWORK_STACK_VAR, &gEfiNetworkStackSetupGuid);
    if (BadSizes[Index] != 0) {
      StorePattern (BadSizes[Index]);
    }

    assert_int_equal (NetworkStackVariableInit (), EFI_SUCCESS);
    ExpectWritten (&Defaults, sizeof (Defaults));
  }

  UseVariable (NETWORK_STACK_VAR, &gEfiNetworkStackSetupGuid);
  StorePattern (sizeof (NETWORK_STACK));
  CopyMem (Before, mVariable.Data, sizeof (Before));
  assert_int_equal (NetworkStackVariableInit (), EFI_SUCCESS);
  assert_int_equal (mVariable.Writes, 0);
  assert_memory_equal (mVariable.Data, Before, sizeof (Before));
}

STATIC VOID
SystemTableSizes (
  VOID  **State
  )
{
  SYSTEM_TABLE  Defaults = { 1 };

  UseVariable (SYSTEM_TABLE_VAR, &gCixGlobalVariableGuid);
  assert_int_equal (SystemTableVariableInit (), EFI_SUCCESS);
  ExpectWritten (&Defaults, sizeof (Defaults));

  // Previously written back with the stored size from a one-byte stack buffer.
  UseVariable (SYSTEM_TABLE_VAR, &gCixGlobalVariableGuid);
  StorePattern (32);
  assert_int_equal (SystemTableVariableInit (), EFI_SUCCESS);
  ExpectWritten (&Defaults, sizeof (Defaults));

  UseVariable (SYSTEM_TABLE_VAR, &gCixGlobalVariableGuid);
  StorePattern (sizeof (SYSTEM_TABLE));
  assert_int_equal (SystemTableVariableInit (), EFI_SUCCESS);
  assert_int_equal (mVariable.Writes, 0);
  assert_int_equal (mVariable.Data[0], 0x80);
}

int
main (
  void
  )
{
  const struct CMUnitTest  Tests[] = {
    cmocka_unit_test (SetupNotFoundCreatesDefaults),
    cmocka_unit_test (SetupExactSizeIsKept),
    cmocka_unit_test (SetupOversizedGetsDefaults),
    cmocka_unit_test (SetupUnknownShortGetsDefaults),
    cmocka_unit_test (Setup2025Q4IsMigrated),
    cmocka_unit_test (SetupReadErrorIsNotOverwritten),
    cmocka_unit_test (NetworkStackSizes),
    cmocka_unit_test (SystemTableSizes),
  };

  return cmocka_run_group_tests (Tests, NULL, NULL);
}
