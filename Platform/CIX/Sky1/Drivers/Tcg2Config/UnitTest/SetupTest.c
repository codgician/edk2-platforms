/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include "SetupAdapter.h"

static EFI_STATUS Callback(UINT16 Id, UINT8 Type, EFI_IFR_TYPE_VALUE *Value) {
  EFI_BROWSER_ACTION_REQUEST Request = 99;
  return Tcg2Callback(&TestPrivate.ConfigAccess, EFI_BROWSER_ACTION_CHANGED, Id, Type, Value, &Request);
}
static void RejectWrongTypes(void **State) {
  EFI_IFR_TYPE_VALUE Value = {0};
  ResetSetup();
  Value.u8 = TCG2_PHYSICAL_PRESENCE_CLEAR;
  assert_int_equal(Callback(KEY_TPM2_OPERATION, EFI_IFR_TYPE_NUM_SIZE_32, &Value), EFI_INVALID_PARAMETER);
  assert_int_equal(SubmitCalls, 0);
}
static void RejectUnsupportedAndDeviceChanges(void **State) {
  EFI_IFR_TYPE_VALUE Value = {0};
  ResetSetup();
  Value.u8 = TCG2_PHYSICAL_PRESENCE_DISABLE;
  assert_int_equal(Callback(KEY_TPM2_OPERATION, EFI_IFR_TYPE_NUM_SIZE_8, &Value), EFI_UNSUPPORTED);
  assert_int_equal(SubmitCalls, 0);
  Value.u8 = TPM_DEVICE_1_2;
  assert_int_equal(Callback(KEY_TPM_DEVICE, EFI_IFR_TYPE_NUM_SIZE_8, &Value), EFI_UNSUPPORTED);
}
static void StageBankRequest(void **State) {
  EFI_IFR_TYPE_VALUE Value = {0};
  ResetSetup();
  Value.u8 = TCG2_PHYSICAL_PRESENCE_SET_PCR_BANKS;
  assert_int_equal(Callback(KEY_TPM2_OPERATION, EFI_IFR_TYPE_NUM_SIZE_8, &Value), EFI_SUCCESS);
  assert_int_equal(SubmitCalls, 0);
  Value.u32 = 3;
  assert_int_equal(Callback(KEY_TPM2_OPERATION_PARAMETER, EFI_IFR_TYPE_NUM_SIZE_32, &Value), EFI_SUCCESS);
  assert_int_equal(SubmitCalls, 1);
  assert_int_equal(SubmitOperation, TCG2_PHYSICAL_PRESENCE_SET_PCR_BANKS);
  assert_int_equal(SubmitParameter, 3);
}
static void RejectInvalidBanksAndRollback(void **State) {
  ResetSetup();
  assert_int_equal(SaveTcg2PCRBanksRequest(4, TRUE), EFI_INVALID_PARAMETER);
  assert_int_equal(SaveTcg2PCRBanksRequest(1, FALSE), EFI_INVALID_PARAMETER);
  assert_int_equal(SubmitCalls, 0);
  SubmitResult = TCG_PP_SUBMIT_REQUEST_TO_PREOS_GENERAL_FAILURE;
  assert_int_equal(SaveTcg2PCRBanksRequest(0, TRUE), EFI_OUT_OF_RESOURCES);
  assert_int_equal(TestPrivate.PCRBanksDesired, 2);
}
static void QueueClearAndRejectParameterConfusion(void **State) {
  EFI_IFR_TYPE_VALUE Value = {0};
  ResetSetup();
  Value.u8 = TCG2_PHYSICAL_PRESENCE_CLEAR;
  assert_int_equal(Callback(KEY_TPM2_OPERATION, EFI_IFR_TYPE_NUM_SIZE_8, &Value), EFI_SUCCESS);
  assert_int_equal(SubmitOperation, TCG2_PHYSICAL_PRESENCE_CLEAR);
  assert_int_equal(SubmitParameter, 0);
  assert_int_equal(SubmitCalls, 1);
  Value.u32 = 3;
  assert_int_equal(Callback(KEY_TPM2_OPERATION_PARAMETER, EFI_IFR_TYPE_NUM_SIZE_32, &Value), EFI_INVALID_PARAMETER);
  assert_int_equal(SubmitCalls, 1);
}
static void RejectInterfaceMutation(void **State) {
  EFI_IFR_TYPE_VALUE Value = {0};
  EFI_BROWSER_ACTION_REQUEST Request = 99;
  ResetSetup();
  assert_int_equal(Tcg2Callback(&TestPrivate.ConfigAccess, EFI_BROWSER_ACTION_CHANGING,
    KEY_TPM_DEVICE_INTERFACE, EFI_IFR_TYPE_NUM_SIZE_8, &Value, &Request), EFI_UNSUPPORTED);
  assert_int_equal(Request, EFI_BROWSER_ACTION_REQUEST_NONE);
}
static void RejectUnhashableNumericBanks(void **State) {
  static const UINT32 Requests[] = {0x10, 0x12, 0x1F};
  EFI_IFR_TYPE_VALUE Value = {0};
  for (UINTN Index = 0; Index < ARRAY_SIZE(Requests); Index++) {
    ResetSetup();
    TestPrivate.ProtocolCapability.HashAlgorithmBitmap = 0xF;
    Value.u8 = TCG2_PHYSICAL_PRESENCE_SET_PCR_BANKS;
    assert_int_equal(Callback(KEY_TPM2_OPERATION, EFI_IFR_TYPE_NUM_SIZE_8, &Value), EFI_SUCCESS);
    Value.u32 = Requests[Index];
    assert_int_equal(Callback(KEY_TPM2_OPERATION_PARAMETER, EFI_IFR_TYPE_NUM_SIZE_32, &Value), EFI_INVALID_PARAMETER);
    assert_int_equal(SubmitCalls, 0);
    assert_int_equal(TestPrivate.PCRBanksDesired, 2);
    assert_int_equal(mCurrentPpRequest, TCG2_PHYSICAL_PRESENCE_SET_PCR_BANKS);
  }
}
static void RejectUnhashableCheckboxBanks(void **State) {
  EFI_IFR_TYPE_VALUE Value = {0};
  ResetSetup();
  TestPrivate.ProtocolCapability.HashAlgorithmBitmap = 0xF;
  Value.b = TRUE;
  assert_int_equal(Callback(KEY_TPM2_PCR_BANKS_REQUEST_4, EFI_IFR_TYPE_BOOLEAN, &Value), EFI_INVALID_PARAMETER);
  assert_int_equal(SubmitCalls, 0);
  assert_int_equal(TestPrivate.PCRBanksDesired, 2);
  assert_int_equal(mCurrentPpRequest, 0);
}
static void HonorCapabilityAndRejectEmptyAllocation(void **State) {
  EFI_IFR_TYPE_VALUE Value = {0};
  for (UINT32 Mask = 0; Mask <= 0xF; Mask++) {
    for (UINT32 Bank = 0; Bank < 4; Bank++) {
      ResetSetup();
      TestPrivate.ProtocolCapability.HashAlgorithmBitmap = Mask;
      TestPrivate.PCRBanksDesired = 0;
      Value.b = TRUE;
      EFI_STATUS Expected = (Mask & (1U << Bank)) ? EFI_SUCCESS : EFI_INVALID_PARAMETER;
      assert_int_equal(Callback(KEY_TPM2_PCR_BANKS_REQUEST_0 + Bank, EFI_IFR_TYPE_BOOLEAN, &Value), Expected);
      assert_int_equal(SubmitCalls, Expected == EFI_SUCCESS ? 1 : 0);
      assert_int_equal(TestPrivate.PCRBanksDesired, Expected == EFI_SUCCESS ? 1U << Bank : 0);
      if (Expected == EFI_SUCCESS) { assert_int_equal(SubmitParameter, 1U << Bank); }
      SubmitCalls = 0;
      mCurrentPpRequest = TCG2_PHYSICAL_PRESENCE_SET_PCR_BANKS;
      Value.u32 = 1U << Bank;
      assert_int_equal(Callback(KEY_TPM2_OPERATION_PARAMETER, EFI_IFR_TYPE_NUM_SIZE_32, &Value), Expected);
      assert_int_equal(SubmitCalls, Expected == EFI_SUCCESS ? 1 : 0);
      if (Expected == EFI_SUCCESS) { assert_int_equal(SubmitParameter, 1U << Bank); }
      SubmitCalls = 0;
      Value.u32 = 0;
      assert_int_equal(Callback(KEY_TPM2_OPERATION_PARAMETER, EFI_IFR_TYPE_NUM_SIZE_32, &Value), EFI_INVALID_PARAMETER);
      TestPrivate.PCRBanksDesired = 1U << Bank;
      Value.b = FALSE;
      assert_int_equal(Callback(KEY_TPM2_PCR_BANKS_REQUEST_0 + Bank, EFI_IFR_TYPE_BOOLEAN, &Value), EFI_INVALID_PARAMETER);
      assert_int_equal(SubmitCalls, 0);
      assert_int_equal(TestPrivate.PCRBanksDesired, 1U << Bank);
    }
  }
}
static void PreserveLiveDesiredBanksUntilAccepted(void **State) {
  EFI_IFR_TYPE_VALUE Value = {0};
  ResetSetup();
  TestPrivate.ProtocolCapability.HashAlgorithmBitmap = 0xF;
  TestPrivate.PCRBanksDesired = 0x12;
  Value.b = TRUE;
  assert_int_equal(Callback(KEY_TPM2_PCR_BANKS_REQUEST_0, EFI_IFR_TYPE_BOOLEAN, &Value), EFI_INVALID_PARAMETER);
  assert_int_equal(SubmitCalls, 0);
  assert_int_equal(TestPrivate.PCRBanksDesired, 0x12);
  Value.b = FALSE;
  assert_int_equal(Callback(KEY_TPM2_PCR_BANKS_REQUEST_4, EFI_IFR_TYPE_BOOLEAN, &Value), EFI_SUCCESS);
  assert_int_equal(SubmitCalls, 1);
  assert_int_equal(SubmitParameter, 2);
  assert_int_equal(TestPrivate.PCRBanksDesired, 2);
  Value.b = TRUE;
  assert_int_equal(Callback(KEY_TPM2_PCR_BANKS_REQUEST_0, EFI_IFR_TYPE_BOOLEAN, &Value), EFI_SUCCESS);
  assert_int_equal(Callback(KEY_TPM2_PCR_BANKS_REQUEST_2, EFI_IFR_TYPE_BOOLEAN, &Value), EFI_SUCCESS);
  assert_int_equal(SubmitParameter, 7);
  assert_int_equal(TestPrivate.PCRBanksDesired, 7);
}
int main(void) {
  const struct CMUnitTest Tests[] = {
    cmocka_unit_test(RejectWrongTypes), cmocka_unit_test(RejectUnsupportedAndDeviceChanges),
    cmocka_unit_test(StageBankRequest), cmocka_unit_test(RejectInvalidBanksAndRollback),
    cmocka_unit_test(QueueClearAndRejectParameterConfusion), cmocka_unit_test(RejectInterfaceMutation),
    cmocka_unit_test(RejectUnhashableNumericBanks), cmocka_unit_test(RejectUnhashableCheckboxBanks),
    cmocka_unit_test(HonorCapabilityAndRejectEmptyAllocation),
    cmocka_unit_test(PreserveLiveDesiredBanksUntilAccepted)
  };
  return cmocka_run_group_tests(Tests, NULL, NULL);
}
