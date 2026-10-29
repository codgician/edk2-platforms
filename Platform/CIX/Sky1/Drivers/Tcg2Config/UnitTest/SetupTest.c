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
int main(void) {
  const struct CMUnitTest Tests[] = {
    cmocka_unit_test(RejectWrongTypes), cmocka_unit_test(RejectUnsupportedAndDeviceChanges),
    cmocka_unit_test(StageBankRequest), cmocka_unit_test(RejectInvalidBanksAndRollback),
    cmocka_unit_test(QueueClearAndRejectParameterConfusion), cmocka_unit_test(RejectInterfaceMutation)
  };
  return cmocka_run_group_tests(Tests, NULL, NULL);
}
