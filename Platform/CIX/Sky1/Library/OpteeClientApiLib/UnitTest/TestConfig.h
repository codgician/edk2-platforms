/** @file
  Host-only platform reservation for synthetic shared memory.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Uefi.h>
extern UINT8 TestPool[1024 * 1024];
#define _PCD_VALUE_PcdTrustZoneSharedMemoryBase  ((UINTN)TestPool)
#define _PCD_VALUE_PcdTrustZoneSharedMemorySize  (1024 * 1024)
#define _PCD_GET_MODE_32_PcdMaximumUnicodeStringLength 0
#define _PCD_GET_MODE_32_PcdMaximumAsciiStringLength 0
