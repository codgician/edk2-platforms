/** @file
  C1 declaration while the OP-TEE slice is not yet merged.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include_next <Library/OpteeClientApiLib.h>
#include <Library/tee_client_api.h>
TEEC_Result OpteeClientCloseSession (IN TEEC_Session *Session);
