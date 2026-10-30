/** @file
  External service mocks. An unscripted service call is a test failure.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Library/ArmMmuLib.h>
#include <Pi/PiFirmwareFile.h>
#include <Library/DxeServicesLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include "TestSupport.h"
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <stdarg.h>

// Only architecture/string base adapters are mocked; formatting is BasePrintLib.
UINT64 EFIAPI DivU64x32Remainder (UINT64 Dividend, UINT32 Divisor, UINT32 *Remainder) {
  if (Remainder != NULL) { *Remainder = Dividend % Divisor; }
  return Dividend / Divisor;
}
UINTN EFIAPI StrnLenS (CONST CHAR16 *String, UINTN MaxSize) {
  UINTN Length;
  for (Length = 0; Length < MaxSize && String[Length] != 0; Length++) { }
  return Length;
}

UINTN TestDiagnosticCount;
CHAR8 TestDiagnostic[512];
EFI_SYSTEM_TABLE *gST;

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

UINT8 TestPool[TEST_POOL_SIZE] __attribute__((aligned(65536)));
UINT64 TestShmBase;
UINT64 TestShmSize;
UINT64 TestShmFlags;
UINTN TestCapabilityStatus;
UINTN TestCapabilityFlags;
UINTN TestConfigStatus;
UINTN TestMapCalls;
EFI_STATUS TestMapStatus;
UINTN TestDelayCalls;
UINTN TestDelayMicroseconds;
UINTN TestTimeCalls;
UINTN TestFvCalls;
VOID (*TestSmcHook)(ARM_SMC_ARGS *Args);
VOID (*TestCacheHook)(ARM_SMC_ARGS *Args);
VOID (*TestFreeHook)(VOID *Buffer);
EFI_STATUS (*TestGetTimeHook)(EFI_TIME *Time);
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
  } else if (Args->Arg0 == OPTEE_SMC_DISABLE_SHM_CACHE) {
    if (TestCacheHook != NULL) { TestCacheHook (Args); }
    else { Args->Arg0 = OPTEE_SMC_RETURN_ENOTAVAIL; }
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
STATIC EFI_STATUS EFIAPI GetTime (EFI_TIME *Time, EFI_TIME_CAPABILITIES *Caps) {
  assert (Caps == NULL && TestGetTimeHook != NULL);
  TestTimeCalls++;
  return TestGetTimeHook (Time);
}
STATIC EFI_RUNTIME_SERVICES mRuntime = { .GetTime = GetTime };
EFI_RUNTIME_SERVICES *gRT = &mRuntime;
UINTN EFIAPI __wrap_MicroSecondDelay (UINTN Microseconds) {
  TestDelayCalls++;
  TestDelayMicroseconds += Microseconds;
  assert (Microseconds <= 100000);
  return Microseconds;
}
// BaseLib architecture helpers do not execute privileged instructions here.
UINT16 EFIAPI SwapBytes16 (UINT16 V) { return __builtin_bswap16 (V); }
UINT32 EFIAPI SwapBytes32 (UINT32 V) { return __builtin_bswap32 (V); }
UINT16 EFIAPI ReadUnaligned16 (CONST UINT16 *P) { UINT16 V; memcpy (&V, P, 2); return V; }
UINT32 EFIAPI ReadUnaligned32 (CONST UINT32 *P) { UINT32 V; memcpy (&V, P, 4); return V; }
UINT16 EFIAPI WriteUnaligned16 (UINT16 *P, UINT16 V) { memcpy (P, &V, 2); return V; }
UINT32 EFIAPI WriteUnaligned32 (UINT32 *P, UINT32 V) { memcpy (P, &V, 4); return V; }

VOID TestResetServices (VOID) {
  TestShmBase = (UINTN)TestPool;
  TestShmSize = TEST_POOL_SIZE;
  TestShmFlags = OPTEE_SMC_SHM_CACHED;
  TestCapabilityStatus = OPTEE_SMC_RETURN_OK;
  TestCapabilityFlags = OPTEE_SMC_SEC_CAP_HAVE_RESERVED_SHM;
  TestConfigStatus = OPTEE_SMC_RETURN_OK;
  TestCacheHook = NULL;
  TestMapCalls = TestDelayCalls = TestDelayMicroseconds = TestTimeCalls = TestFvCalls = 0;
  TestMapStatus = EFI_SUCCESS;
  TestSmcHook = NULL;
  TestFreeHook = NULL;
  TestGetTimeHook = NULL;
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
  assert (OpteeClientApiFinalize ());
  assert (HostLivePoolAllocations () == mInitialPools);
  TestResetServices ();
}
