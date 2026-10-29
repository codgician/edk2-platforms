/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <stdint.h>
#include <stddef.h>
#include "SetupAdapter.h"
int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
  EFI_IFR_TYPE_VALUE Value;
  EFI_BROWSER_ACTION_REQUEST Request;
  UINT16 Id;
  if (Size < 5) { return 0; }
  ResetSetup();
  ZeroMem(&Value, sizeof(Value));
  CopyMem(&Value, Data + 4, MIN(Size - 4, sizeof(Value)));
  Id = (UINT16)(KEY_TPM_DEVICE + Data[0] % 11);
  Tcg2Callback(&TestPrivate.ConfigAccess,
    (Data[1] & 1) ? EFI_BROWSER_ACTION_CHANGED : EFI_BROWSER_ACTION_CHANGING,
    Id, Data[2], &Value, &Request);
  if (SubmitCalls != 0) {
    if (((Data[1] & 1) == 0) || (SubmitCalls != 1)) { abort(); }
    if (Id == KEY_TPM2_OPERATION) {
      if (Data[2] != EFI_IFR_TYPE_NUM_SIZE_8) { abort(); }
    } else if ((Id >= KEY_TPM2_PCR_BANKS_REQUEST_0) && (Id <= KEY_TPM2_PCR_BANKS_REQUEST_4)) {
      if (Data[2] != EFI_IFR_TYPE_BOOLEAN) { abort(); }
    } else { abort(); }
    switch (SubmitOperation) {
      case TCG2_PHYSICAL_PRESENCE_NO_ACTION:
      case TCG2_PHYSICAL_PRESENCE_CLEAR:
      case TCG2_PHYSICAL_PRESENCE_ENABLE_CLEAR:
      case TCG2_PHYSICAL_PRESENCE_ENABLE_CLEAR_2:
      case TCG2_PHYSICAL_PRESENCE_ENABLE_CLEAR_3:
      case TCG2_PHYSICAL_PRESENCE_CHANGE_EPS:
      case TCG2_PHYSICAL_PRESENCE_LOG_ALL_DIGESTS:
        if (SubmitParameter != 0) { abort(); }
        break;
      case TCG2_PHYSICAL_PRESENCE_SET_PCR_BANKS:
        if ((SubmitParameter == 0) || ((SubmitParameter & ~3U) != 0)) { abort(); }
        break;
      default: abort();
    }
  }
  if (Request != EFI_BROWSER_ACTION_REQUEST_NONE) { abort(); }
  return 0;
}
