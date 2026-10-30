/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <stdint.h>
#include <stddef.h>
#include "SetupAdapter.h"
int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
  EFI_IFR_TYPE_VALUE Value;
  EFI_BROWSER_ACTION_REQUEST Request;
  UINT16 Id;
  EFI_STATUS Status;
  UINT32 Allowed;
  UINT32 DesiredBefore;
  TCG2_CONFIGURATION_INFO Info;
  static const UINT32 Algorithms[] = {TPM_ALG_SHA1, TPM_ALG_SHA256, TPM_ALG_SHA384, TPM_ALG_SHA512, TPM_ALG_SM3_256};
  if (Size < 10) { return 0; }
  ResetSetup();
  TestPrivate.ProtocolCapability.HashAlgorithmBitmap = Data[4];
  TestPcdTcg2HashAlgorithmBitmap = Data[5];
  TestPcdTpm2HashMask = Data[6];
  TestPrivate.PCRBanksDesired = Data[7];
  DesiredBefore = TestPrivate.PCRBanksDesired;
  Allowed = 7U & Data[4] & Data[5] & Data[6];
  ZeroMem(&Info, sizeof(Info));
  for (UINTN Index = 0; Index < ARRAY_SIZE(Algorithms); Index++) { SetConfigInfo(&Info, Algorithms[Index]); }
  if ((Info.Sha1Supported != ((Allowed & 1U) != 0)) ||
      (Info.Sha256Supported != ((Allowed & 2U) != 0)) ||
      (Info.Sha384Supported != ((Allowed & 4U) != 0)) ||
      Info.Sha512Supported || Info.Sm3Supported) { abort(); }
  if ((Data[3] & 1) != 0) {
    if (SaveTcg2PpRequest(TCG2_PHYSICAL_PRESENCE_SET_PCR_BANKS) != EFI_SUCCESS) { abort(); }
    if (SubmitCalls != 0) { abort(); }
  }
  ZeroMem(&Value, sizeof(Value));
  CopyMem(&Value, Data + 9, MIN(Size - 9, sizeof(Value)));
  Id = (UINT16)(KEY_TPM_DEVICE + Data[0] % 11);
  Status = Tcg2Callback(&TestPrivate.ConfigAccess,
    (Data[1] & 1) ? EFI_BROWSER_ACTION_CHANGED : EFI_BROWSER_ACTION_CHANGING,
    Id, Data[2], &Value, &Request);
  if (SubmitCalls != 0) {
    if (((Data[1] & 1) == 0) || (SubmitCalls != 1)) { abort(); }
    if (Id == KEY_TPM2_OPERATION) {
      if (Data[2] != EFI_IFR_TYPE_NUM_SIZE_8) { abort(); }
    } else if (Id == KEY_TPM2_OPERATION_PARAMETER) {
      if (Data[2] != EFI_IFR_TYPE_NUM_SIZE_32) { abort(); }
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
        if ((SubmitParameter == 0) || ((SubmitParameter & ~Allowed) != 0)) { abort(); }
        break;
      default: abort();
    }
  }
  if (EFI_ERROR(Status) && (TestPrivate.PCRBanksDesired != DesiredBefore)) { abort(); }
  if ((Id >= KEY_TPM2_PCR_BANKS_REQUEST_0) && (Id <= KEY_TPM2_PCR_BANKS_REQUEST_4) && !EFI_ERROR(Status)) {
    if (TestPrivate.PCRBanksDesired != SubmitParameter) { abort(); }
  }
  if (Request != EFI_BROWSER_ACTION_REQUEST_NONE) { abort(); }
  return 0;
}
