/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <PiPei.h>
#include <Library/PcdLib.h>
#include <Library/BaseMemoryLib.h>
static EFI_GUID Selected;
static EFI_STATUS SelectGuid(UINTN *Size, VOID *Data) {
  assert_int_equal(*Size, sizeof(Selected));
  CopyMem(&Selected, Data, *Size);
  return EFI_SUCCESS;
}
#undef PcdSetPtrS
#undef PcdGetPtr
#undef PcdGetBool
#define PcdSetPtrS(Token, Size, Buffer) SelectGuid(Size, Buffer)
#define PcdGetPtr(Token) (&Selected)
#define PcdGetBool(Token) FALSE
#include "../Tcg2ConfigPeim.c"
EFI_GUID gEfiTpmDeviceSelectedGuid;
EFI_GUID gPeiTpmInitializationDonePpiGuid;
EFI_GUID gEfiTpmDeviceInstanceNoneGuid = TPM_DEVICE_INTERFACE_NONE;
EFI_GUID gEfiPeiReadOnlyVariable2PpiGuid;
EFI_GUID gTcg2ConfigFormSetGuid;
EFI_STATUS EFIAPI PeiServicesLocatePpi(CONST EFI_GUID *Guid, UINTN Instance, EFI_PEI_PPI_DESCRIPTOR **Descriptor, VOID **Ppi) {
  fail_msg("Fixed fTPM selection must not consult mutable setup variables");
  return EFI_NOT_FOUND;
}
EFI_STATUS EFIAPI PeiServicesInstallPpi(CONST EFI_PEI_PPI_DESCRIPTOR *Ppi) {
  assert_ptr_equal(Ppi, &gTpmSelectedPpi);
  return EFI_SUCCESS;
}
UINT8 DetectTpmDevice(UINT8 Device) { fail_msg("Fixed selection must not probe dTPM"); return 0; }
static void FixedTpm20(void **State) {
  EFI_GUID Expected = TPM_DEVICE_INTERFACE_TPM20_DTPM;
  ZeroMem(&Selected, sizeof(Selected));
  assert_int_equal(Tcg2ConfigPeimEntryPoint(NULL, NULL), EFI_SUCCESS);
  assert_memory_equal(&Selected, &Expected, sizeof(Expected));
}
int main(void) {
  const struct CMUnitTest Tests[] = { cmocka_unit_test(FixedTpm20) };
  return cmocka_run_group_tests(Tests, NULL, NULL);
}
