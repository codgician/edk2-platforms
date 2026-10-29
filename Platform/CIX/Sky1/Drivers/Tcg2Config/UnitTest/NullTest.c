/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <Library/Tcg2PhysicalPresenceLib.h>
static void NoImplementedOperations(void **State) {
  UINT32 Operation;
  UINT32 Parameter = 0x1234;
  UINT32 Response = 0x5678;
  for (Operation = 0; Operation <= 256; Operation++) {
    assert_int_equal(Tcg2PhysicalPresenceLibSubmitRequestToPreOSFunction(Operation, 0), TCG_PP_SUBMIT_REQUEST_TO_PREOS_NOT_IMPLEMENTED);
    assert_int_equal(Tcg2PhysicalPresenceLibGetUserConfirmationStatusFunction(Operation), TCG_PP_GET_USER_CONFIRMATION_NOT_IMPLEMENTED);
    assert_int_equal(Tcg2PhysicalPresenceLibSubmitRequestToPreOSFunctionEx(&Operation, &Parameter), TCG_PP_SUBMIT_REQUEST_TO_PREOS_NOT_IMPLEMENTED);
    assert_int_equal(Parameter, 0x1234);
  }
  assert_false(Tcg2PhysicalPresenceLibNeedUserConfirm());
  assert_int_equal(Tcg2PhysicalPresenceLibReturnOperationResponseToOsFunction(&Parameter, &Response), TCG_PP_RETURN_TPM_OPERATION_RESPONSE_FAILURE);
  assert_int_equal(Parameter, 0x1234);
  assert_int_equal(Response, 0x5678);
  Tcg2PhysicalPresenceLibProcessRequest(NULL);
  assert_int_equal(Tcg2PhysicalPresenceLibGetManagementFlags(),
    TCG2_BIOS_TPM_MANAGEMENT_FLAG_PP_REQUIRED_FOR_CLEAR |
    TCG2_BIOS_TPM_MANAGEMENT_FLAG_PP_REQUIRED_FOR_TURN_ON |
    TCG2_BIOS_TPM_MANAGEMENT_FLAG_PP_REQUIRED_FOR_TURN_OFF |
    TCG2_BIOS_TPM_MANAGEMENT_FLAG_PP_REQUIRED_FOR_CHANGE_EPS |
    TCG2_BIOS_TPM_MANAGEMENT_FLAG_PP_REQUIRED_FOR_CHANGE_PCRS);
}
int main(void) {
  const struct CMUnitTest Tests[] = { cmocka_unit_test(NoImplementedOperations) };
  return cmocka_run_group_tests(Tests, NULL, NULL);
}
