/** @file
  Stand-in for the build tool's AutoGen.h when compiling firmware sources
  for the host. Tests provide module-specific PCDs and globals themselves
  (see PRELUDE in the harness). ASSERT() stays active and aborts the test;
  the host DebugLib reports DEBUG() and DEBUG_CODE() disabled, so they
  compile but never run.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef SKY1_HOST_AUTOGEN_H_
#define SKY1_HOST_AUTOGEN_H_

#include <Uefi.h>

#endif
