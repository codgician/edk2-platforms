/** @file
  External service mocks. An unscripted service call is a test failure.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Library/ArmMmuLib.h>
#include <Pi/PiFirmwareFile.h>
#include <Library/DxeServicesLib.h>
#include "TestSupport.h"
// Production never finalizes; one unit lets TestFini reset its private state.
#include "../OpteeClientApiLib.c"
#include "../OpteeClientRpc.c"
#include "../OpteeReeFs.c"
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <stdarg.h>

UINTN TestDiagnosticCount;
CHAR8 TestDiagnostic[512];
EFI_SYSTEM_TABLE *gST;
EFI_GUID gOpteeFtpmTaGuid = { 0xbc50d971, 0xd4c9, 0x42c4, { 0x82, 0xcb, 0x34, 0x3f, 0xb7, 0xf3, 0x78, 0x96 } };

VOID TestClearDiagnostics (VOID) {
  TestDiagnosticCount = 0;
  ZeroMem (TestDiagnostic, sizeof (TestDiagnostic));
}
UINTN EFIAPI SerialPortWrite (UINT8 *Buffer, UINTN Size) {
  assert (Size < sizeof (TestDiagnostic));
  assert (Size >= 2 && Buffer[Size - 2] == '\r' && Buffer[Size - 1] == '\n');
  memcpy (TestDiagnostic, Buffer, Size);
  TestDiagnostic[Size] = 0;
  TestDiagnosticCount++;
  assert (strstr (TestDiagnostic, "SECRET-CANARY") == NULL);
  return Size;
}
VOID TestExpectDiagnostic (CONST CHAR8 *Check, UINT64 Value, UINT64 Status) {
  CHAR8 Expected[512];
  snprintf (Expected, sizeof (Expected), "TPM OpteeClient: %s value=0x%llX (0x%llX)\r\n",
            Check, (unsigned long long)Value, (unsigned long long)Status);
  assert (TestDiagnosticCount == 1);
  assert (strcmp (TestDiagnostic, Expected) == 0);
  TestClearDiagnostics ();
}
// For checks whose value is metadata the test does not pin.
VOID TestExpectCheck (CONST CHAR8 *Check) {
  CHAR8 Prefix[256];
  snprintf (Prefix, sizeof (Prefix), "TPM OpteeClient: %s value=0x", Check);
  assert (TestDiagnosticCount == 1 && strncmp (TestDiagnostic, Prefix, strlen (Prefix)) == 0);
  TestClearDiagnostics ();
}

UINT8 TestPool[TEST_POOL_SIZE] __attribute__((aligned(65536)));
UINT64 TestShmBase;
UINT64 TestShmSize;
UINT64 TestShmFlags;
UINTN TestCapabilityStatus;
UINTN TestCapabilityFlags;
UINTN TestConfigStatus;
UINTN TestMapCalls;
EFI_STATUS TestMapStatus;
UINTN TestFvCalls;
VOID (*TestSmcHook)(ARM_SMC_ARGS *Args);
VOID (*TestFreeHook)(VOID *Buffer);
EFI_STATUS (*TestFvHook)(CONST EFI_GUID *Guid, VOID **Image, UINTN *Size);
STATIC UINTN mFailAllocation;
STATIC INTN mInitialPools;

VOID TestFailAllocation (UINTN N) { mFailAllocation = N; }
STATIC BOOLEAN FailPool (UINTN Size) {
  if ((mFailAllocation != 0) && (--mFailAllocation == 0)) { return TRUE; }
  return Size > 8 * 1024 * 1024;
}
VOID *EFIAPI __real_AllocatePool (UINTN Size);
VOID EFIAPI __real_FreePool (VOID *Buffer);
VOID EFIAPI __wrap_FreePool (VOID *Buffer) {
  if (TestFreeHook != NULL) { TestFreeHook (Buffer); }
  __real_FreePool (Buffer);
}
VOID *EFIAPI __wrap_AllocatePool (UINTN Size) {
  return FailPool (Size) ? NULL : __real_AllocatePool (Size);
}
VOID *EFIAPI __wrap_AllocateZeroPool (UINTN Size) {
  VOID *Buffer = __wrap_AllocatePool (Size);
  if (Buffer != NULL) { ZeroMem (Buffer, Size); }
  return Buffer;
}
VOID *EFIAPI __wrap_ReallocatePool (UINTN OldSize, UINTN NewSize, VOID *Old) {
  VOID *Buffer = __wrap_AllocateZeroPool (NewSize);
  if ((Buffer != NULL) && (Old != NULL)) {
    CopyMem (Buffer, Old, MIN (OldSize, NewSize));
    FreePool (Old);
  }
  return Buffer;
}
VOID EFIAPI ArmCallSmc (ARM_SMC_ARGS *Args) {
  if (Args->Arg0 == OPTEE_SMC_EXCHANGE_CAPABILITIES) {
    Args->Arg0 = TestCapabilityStatus;
    Args->Arg1 = TestCapabilityFlags;
  } else if (Args->Arg0 == OPTEE_SMC_GET_SHM_CONFIG) {
    Args->Arg0 = TestConfigStatus;
    Args->Arg1 = TestShmBase;
    Args->Arg2 = TestShmSize;
    Args->Arg3 = TestShmFlags;
  } else {
    assert (TestSmcHook != NULL);
    TestSmcHook (Args);
  }
}
EFI_STATUS ArmSetMemoryAttributes (EFI_PHYSICAL_ADDRESS Base, UINT64 Length, UINT64 Attrs, UINT64 Mask) {
  assert (Base >= (UINTN)TestPool && Length <= TEST_POOL_SIZE);
  assert (Base - (UINTN)TestPool <= TEST_POOL_SIZE - Length);
  assert (Attrs == EFI_MEMORY_WB || Attrs == EFI_MEMORY_UC);
  assert (Mask == 0);
  TestMapCalls++;
  return TestMapStatus;
}
EFI_STATUS EFIAPI GetSectionFromAnyFv (CONST EFI_GUID *Guid, EFI_SECTION_TYPE Type, UINTN Instance, VOID **Image, UINTN *Size) {
  assert (Type == EFI_SECTION_RAW && Instance == 0);
  assert (TestFvHook != NULL);
  TestFvCalls++;
  return TestFvHook (Guid, Image, Size);
}
VOID TestResetServices (VOID) {
  TestShmBase = (UINTN)TestPool;
  TestShmSize = TEST_POOL_SIZE;
  TestShmFlags = OPTEE_SMC_SHM_CACHED;
  TestCapabilityStatus = OPTEE_SMC_RETURN_OK;
  TestCapabilityFlags = OPTEE_SMC_SEC_CAP_HAVE_RESERVED_SHM;
  TestConfigStatus = OPTEE_SMC_RETURN_OK;
  TestMapCalls = TestFvCalls = 0;
  TestMapStatus = EFI_SUCCESS;
  TestSmcHook = NULL;
  TestFreeHook = NULL;
  TestFvHook = NULL;
  TestFailAllocation (0);
  TestClearDiagnostics ();
}
VOID TestInit (VOID) {
  TestResetServices ();
  mInitialPools = HostLivePoolAllocations ();
  memset (TestPool, 0xA7, sizeof (TestPool));
  assert (OpteeClientApiInitialize (NULL) == EFI_SUCCESS);
}
VOID TestFini (VOID) {
  for (UINTN Index = 0; Index < REE_FS_MAX_FILES; Index++) { Clear (&mFiles[Index]); }
  if (mShm != NULL) { FreePool (mShm); }
  mShm = NULL;
  mInitialized = mBroken = FALSE;
  assert (HostLivePoolAllocations () == mInitialPools);
  TestResetServices ();
}
