/** @file
  Private, allocation-free OP-TEE rejection diagnostics and handoff switch.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#ifndef OPTEE_CLIENT_DIAGNOSTICS_H_
#define OPTEE_CLIENT_DIAGNOSTICS_H_

#include <Uefi.h>

/** Permanently suppress console services before ExitBootServices cleanup. */
VOID
OpteeClientApiExitBootServices (
  VOID
  );

/**
  Emit one RELEASE-visible line at the failing check. Check is a trusted
  literal and Value/Status are numeric metadata only, never peer-controlled
  strings or buffer contents.
  After the sticky EBS handoff, this uses stack storage and SerialPortLib only.
**/
VOID
OpteeReject (
  IN CONST CHAR8  *Check,
  IN UINT64       Value,
  IN UINT64       Status
  );

#endif
