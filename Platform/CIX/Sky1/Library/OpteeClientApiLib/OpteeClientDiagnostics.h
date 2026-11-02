/** @file
  Private, allocation-free OP-TEE rejection diagnostics and handoff switch.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#ifndef OPTEE_CLIENT_DIAGNOSTICS_H_
#define OPTEE_CLIENT_DIAGNOSTICS_H_

#include <Uefi.h>

/** First-failure record kept in fixed module storage, never allocated. */
typedef struct {
  CONST CHAR8  *FirstCheck;  ///< Literal of the first OpteeReject, or NULL.
  UINT64       FirstValue;
  UINT64       FirstStatus;
  UINT32       Rejects;      ///< OpteeReject calls, saturating.
  BOOLEAN      Broken;       ///< A suspended secure thread was abandoned.
  UINT32       BrokenCall;   ///< 1-based OP-TEE call ordinal that broke it.
  UINT32       BrokenRpcs;   ///< RPCs serviced during that call.
} OPTEE_CLIENT_DIAGNOSTICS;

/** Copy the record; plain memory reads only, safe in EBS callbacks. */
VOID
OpteeClientGetDiagnostics (
  OUT OPTEE_CLIENT_DIAGNOSTICS  *Diagnostics
  );

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
