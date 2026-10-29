/** @file
  Test-side view of the host support library.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef SKY1_HOST_SUPPORT_H_
#define SKY1_HOST_SUPPORT_H_

#include <Uefi.h>

/**
  Number of pool allocations made through MemoryAllocationLib that have not
  been freed. Compare before and after a scenario to detect leaks.
**/
INTN
HostLivePoolAllocations (
  VOID
  );

#endif
