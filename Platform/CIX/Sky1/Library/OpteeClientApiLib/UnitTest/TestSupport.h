/** @file
  Scripted services for the real OP-TEE client host tests.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#ifndef OPTEE_TEST_SUPPORT_H_
#define OPTEE_TEST_SUPPORT_H_

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/OpteeClientApiLib.h>
#include "../OpteeClientInternal.h"
#include "HostSupport.h"

#define TEST_POOL_SIZE  (1024 * 1024)
extern UINT8  TestPool[TEST_POOL_SIZE];
extern UINT64 TestShmBase;
extern UINT64 TestShmSize;
extern UINT64 TestShmFlags;
extern UINTN TestCapabilityStatus;
extern UINTN TestCapabilityFlags;
extern UINTN TestConfigStatus;
extern UINTN  TestMapCalls;
extern EFI_STATUS TestMapStatus;
extern UINTN TestDelayCalls;
extern UINTN TestDelayMicroseconds;
extern UINTN TestTimeCalls;
extern UINTN TestFvCalls;
extern VOID (*TestSmcHook)(ARM_SMC_ARGS *Args);
extern VOID (*TestCacheHook)(ARM_SMC_ARGS *Args);
extern VOID (*TestFreeHook)(VOID *Buffer);
extern EFI_STATUS (*TestGetTimeHook)(EFI_TIME *Time);
extern EFI_STATUS (*TestFvHook)(CONST EFI_GUID *Guid, VOID **Image, UINTN *Size);
extern UINTN TestDiagnosticCount;
extern CHAR8 TestDiagnostic[512];
VOID TestClearDiagnostics (VOID);
VOID TestExpectDiagnostic (CONST CHAR8 *Check, UINT64 Value, UINT64 Status);

VOID TestInit (VOID);
VOID TestFini (VOID);
VOID TestFailAllocation (UINTN N);
VOID TestResetServices (VOID);

#endif
