/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <stdlib.h>
#include "SetupTokens.h"
#include "../Tcg2ConfigImpl.h"
#include <Library/Tpm2Fifo.h>
#include <Library/IoLib.h>
#include <Library/Tpm2CommandLib.h>
#undef PcdGet8
#undef PcdGet32
#undef PcdGet64
#undef PcdGetPtr
#undef FixedPcdGetBool
#define PcdGet8(Token) ((UINT8)0)
#define PcdGet32(Token) ((UINT32)0)
#define PcdGet64(Token) ((UINT64)0)
#define PcdGetPtr(Token) ((VOID *)"1.3")
#define FixedPcdGetBool(Token) FALSE
#include "../Tcg2ConfigImpl.c"

static UINTN SubmitCalls;
static UINT32 SubmitOperation;
static UINT32 SubmitParameter;
static UINT32 SubmitResult;
UINT32 EFIAPI Tcg2PhysicalPresenceLibSubmitRequestToPreOSFunction(UINT32 Operation, UINT32 Parameter) {
  SubmitCalls++;
  SubmitOperation = Operation;
  SubmitParameter = Parameter;
  return SubmitResult;
}
EFI_STATUS EFIAPI Tpm2GetCapabilityManufactureID(UINT32 *Id) { abort(); }
EFI_STATUS EFIAPI Tpm2GetCapabilityFirmwareVersion(UINT32 *First, UINT32 *Second) { abort(); }
EFI_STRING_ID EFIAPI HiiSetString(EFI_HII_HANDLE Handle, EFI_STRING_ID Id, CONST EFI_STRING String, CONST CHAR8 *Languages) { abort(); }
VOID EFIAPI CreatePopUp(UINTN Attribute, EFI_INPUT_KEY *Key, ...) { abort(); }
EFI_STATUS Tpm2ActiveLocality(VOID) { abort(); }
EFI_STATUS EFIAPI Tpm2RegisterRead(UINT32 Offset, UINT8 Length, UINT8 *Data) { abort(); }
EFI_STATUS EFIAPI Tpm2RegisterWrite(UINT32 Offset, UINT8 Length, UINT32 Data) { abort(); }
EFI_STATUS Tpm2WaitRegisterBits(UINT32 Offset, UINT32 Set, UINT32 Clear, UINT32 Timeout) { abort(); }
UINTN EFIAPI AsciiStrSize(CONST CHAR8 *String) { abort(); }
UINT32 EFIAPI MmioRead32(UINTN Address) { abort(); }
UINT32 EFIAPI MmioWrite32(UINTN Address, UINT32 Value) { abort(); }
RETURN_STATUS EFIAPI StrCatS(CHAR16 *Destination, UINTN Maximum, CONST CHAR16 *Source) { abort(); }
UINTN EFIAPI AsciiSPrint(CHAR8 *Buffer, UINTN Size, CONST CHAR8 *Format, ...) { abort(); }
UINTN EFIAPI UnicodeSPrint(CHAR16 *Buffer, UINTN Size, CONST CHAR16 *Format, ...) { abort(); }
EFI_STATUS EFIAPI Tpm2GetCapabilityPcrs(TPML_PCR_SELECTION *Pcrs) { abort(); }
EFI_STATUS EFIAPI Tpm2GetCapabilityIsCommandImplemented(TPM_CC Command, BOOLEAN *Implemented) { abort(); }
EFI_HII_HANDLE EFIAPI HiiAddPackages(CONST EFI_GUID *Guid, EFI_HANDLE Handle, ...) { abort(); }
VOID EFIAPI HiiRemovePackages(EFI_HII_HANDLE Handle) { abort(); }
EFI_BOOT_SERVICES *gBS;
EFI_RUNTIME_SERVICES *gRT;
EFI_GUID gEfiDevicePathProtocolGuid;
EFI_GUID gEfiHiiConfigAccessProtocolGuid;
EFI_GUID gTcg2ConfigFormSetGuid;
UINT8 Tcg2ConfigBin[4];
UINT8 Tcg2ConfigDxeStrings[4];

static TCG2_CONFIG_PRIVATE_DATA TestPrivate;
static VOID ResetSetup(VOID) {
  ZeroMem(&TestPrivate, sizeof(TestPrivate));
  TestPrivate.Signature = TCG2_CONFIG_PRIVATE_DATA_SIGNATURE;
  TestPrivate.PCRBanksDesired = 2;
  TestPrivate.ProtocolCapability.HashAlgorithmBitmap = 3;
  mTcg2ConfigPrivateDate = &TestPrivate;
  mCurrentPpRequest = 0;
  SubmitCalls = 0;
  SubmitResult = TCG_PP_SUBMIT_REQUEST_TO_PREOS_SUCCESS;
}
