/** @file
  The module entry point for Tcg2 configuration module.

Copyright (c) 2015 - 2018, Intel Corporation. All rights reserved.<BR>
Copyright 2024 Cix Technology Group Co., Ltd. All Rights Reserved.
SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "Tcg2ConfigImpl.h"
#include <Library/Tpm2DeviceLib.h>


/**
  Update default PCR banks data.

  @param[in]  HiiPackage        HII Package.
  @param[in]  HiiPackageSize    HII Package size.
  @param[in]  PCRBanks          PCR Banks data.

**/
VOID
UpdateDefaultPCRBanks (
  IN VOID    *HiiPackage,
  IN UINTN   HiiPackageSize,
  IN UINT32  PCRBanks
  )
{
  EFI_HII_PACKAGE_HEADER  *HiiPackageHeader;
  EFI_IFR_OP_HEADER       *IfrOpCodeHeader;
  EFI_IFR_CHECKBOX        *IfrCheckBox;
  EFI_IFR_DEFAULT         *IfrDefault;

  HiiPackageHeader = (EFI_HII_PACKAGE_HEADER *)HiiPackage;

  switch (HiiPackageHeader->Type) {
    case EFI_HII_PACKAGE_FORMS:
      IfrOpCodeHeader = (EFI_IFR_OP_HEADER *)(HiiPackageHeader + 1);
      while ((UINTN)IfrOpCodeHeader < (UINTN)HiiPackageHeader + HiiPackageHeader->Length) {
        switch (IfrOpCodeHeader->OpCode) {
          case EFI_IFR_CHECKBOX_OP:
            IfrCheckBox = (EFI_IFR_CHECKBOX *)IfrOpCodeHeader;
            if ((IfrCheckBox->Question.QuestionId >= KEY_TPM2_PCR_BANKS_REQUEST_0) && (IfrCheckBox->Question.QuestionId <= KEY_TPM2_PCR_BANKS_REQUEST_4)) {
              IfrDefault = (EFI_IFR_DEFAULT *)(IfrCheckBox + 1);
              ASSERT (IfrDefault->Header.OpCode == EFI_IFR_DEFAULT_OP);
              ASSERT (IfrDefault->Type == EFI_IFR_TYPE_BOOLEAN);
              IfrDefault->Value.b = (BOOLEAN)((PCRBanks >> (IfrCheckBox->Question.QuestionId - KEY_TPM2_PCR_BANKS_REQUEST_0)) & 0x1);
            }

            break;
        }

        IfrOpCodeHeader = (EFI_IFR_OP_HEADER *)((UINTN)IfrOpCodeHeader + IfrOpCodeHeader->Length);
      }

      break;
  }

  return;
}

/** Display the build-selected PPI and ACPI versions, never mutable setup data. **/
VOID
InitializeTcg2VersionInfo (
  IN TCG2_CONFIG_PRIVATE_DATA  *PrivateData
  )
{
  UINT64  PcdTcg2PpiVersion;
  UINT8   PcdTpm2AcpiTableRev;

  PcdTcg2PpiVersion = 0;
  CopyMem (&PcdTcg2PpiVersion, PcdGetPtr (PcdTcgPhysicalPresenceInterfaceVer),
           MIN (AsciiStrSize ((CHAR8 *)PcdGetPtr (PcdTcgPhysicalPresenceInterfaceVer)), sizeof (PcdTcg2PpiVersion)));

  switch (PcdTcg2PpiVersion) {
    case TCG2_PPI_VERSION_1_2:
      HiiSetString (PrivateData->HiiHandle, STRING_TOKEN (STR_TCG2_PPI_VERSION_STATE_CONTENT), L"1.2", NULL);
      break;
    case TCG2_PPI_VERSION_1_3:
      HiiSetString (PrivateData->HiiHandle, STRING_TOKEN (STR_TCG2_PPI_VERSION_STATE_CONTENT), L"1.3", NULL);
      break;
    default:
      ASSERT (FALSE);
      break;
  }

  PcdTpm2AcpiTableRev = PcdGet8 (PcdTpm2AcpiTableRev);

  switch (PcdTpm2AcpiTableRev) {
    case EFI_TPM2_ACPI_TABLE_REVISION_3:
      HiiSetString (PrivateData->HiiHandle, STRING_TOKEN (STR_TPM2_ACPI_REVISION_STATE_CONTENT), L"Rev 3", NULL);
      break;
    case EFI_TPM2_ACPI_TABLE_REVISION_4:
      HiiSetString (PrivateData->HiiHandle, STRING_TOKEN (STR_TPM2_ACPI_REVISION_STATE_CONTENT), L"Rev 4", NULL);
      break;
    default:
      ASSERT (FALSE);
      break;
  }
}

/**
  The entry point for Tcg2 configuration driver.

  @param[in]  ImageHandle        The image handle of the driver.
  @param[in]  SystemTable        The system table.

  @retval EFI_ALREADY_STARTED    The driver already exists in system.
  @retval EFI_OUT_OF_RESOURCES   Fail to execute entry point due to lack of resources.
  @retval EFI_SUCCESS            All the related protocols are installed on the driver.
  @retval Others                 Fail to install protocols as indicated.

**/
EFI_STATUS
EFIAPI
Tcg2ConfigDriverEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                    Status;
  TCG2_CONFIG_PRIVATE_DATA      *PrivateData;
  UINT32                        CurrentActivePCRBanks;

  Tpm2RequestUseTpm ();

  Status = gBS->OpenProtocol (
                  ImageHandle,
                  &gEfiCallerIdGuid,
                  NULL,
                  ImageHandle,
                  ImageHandle,
                  EFI_OPEN_PROTOCOL_TEST_PROTOCOL
                  );
  if (!EFI_ERROR (Status)) {
    return EFI_ALREADY_STARTED;
  }

  //
  // Create a private data structure.
  //
  PrivateData = AllocateCopyPool (sizeof (TCG2_CONFIG_PRIVATE_DATA), &mTcg2ConfigPrivateDateTemplate);
  ASSERT (PrivateData != NULL);
  mTcg2ConfigPrivateDate = PrivateData;
  //
  // Install private GUID.
  //
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &ImageHandle,
                  &gEfiCallerIdGuid,
                  PrivateData,
                  NULL
                  );
  ASSERT_EFI_ERROR (Status);

  Status = gBS->LocateProtocol (&gEfiTcg2ProtocolGuid, NULL, (VOID **)&PrivateData->Tcg2Protocol);
  ASSERT_EFI_ERROR (Status);

  PrivateData->ProtocolCapability.Size = sizeof (PrivateData->ProtocolCapability);
  Status                               = PrivateData->Tcg2Protocol->GetCapability (
                                                                      PrivateData->Tcg2Protocol,
                                                                      &PrivateData->ProtocolCapability
                                                                      );
  ASSERT_EFI_ERROR (Status);


  //
  // Set value for Tcg2CurrentActivePCRBanks
  // Search Tcg2ConfigBin[] and update default value there
  //
  Status = PrivateData->Tcg2Protocol->GetActivePcrBanks (PrivateData->Tcg2Protocol, &CurrentActivePCRBanks);
  ASSERT_EFI_ERROR (Status);
  PrivateData->PCRBanksDesired = CurrentActivePCRBanks;
  UpdateDefaultPCRBanks (Tcg2ConfigBin + sizeof (UINT32), ReadUnaligned32 ((UINT32 *)Tcg2ConfigBin) - sizeof (UINT32), CurrentActivePCRBanks);
  // TPM2 is fixed; writable legacy setup variables are not an authority.
  PrivateData->TpmDeviceDetected = TPM_DEVICE_2_0_DTPM;


  //
  // Install Tcg2 configuration form
  //
  Status = InstallTcg2ConfigForm (PrivateData);
  if (EFI_ERROR (Status)) {
    goto ErrorExit;
  }

  InitializeTcg2VersionInfo (PrivateData);

  return EFI_SUCCESS;

ErrorExit:
  if (PrivateData != NULL) {
    UninstallTcg2ConfigForm (PrivateData);
  }

  return Status;
}

/**
  Unload the Tcg2 configuration form.

  @param[in]  ImageHandle         The driver's image handle.

  @retval     EFI_SUCCESS         The Tcg2 configuration form is unloaded.
  @retval     Others              Failed to unload the form.

**/
EFI_STATUS
EFIAPI
Tcg2ConfigDriverUnload (
  IN EFI_HANDLE  ImageHandle
  )
{
  EFI_STATUS                Status;
  TCG2_CONFIG_PRIVATE_DATA  *PrivateData;

  Status = gBS->HandleProtocol (
                  ImageHandle,
                  &gEfiCallerIdGuid,
                  (VOID **)&PrivateData
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ASSERT (PrivateData->Signature == TCG2_CONFIG_PRIVATE_DATA_SIGNATURE);

  gBS->UninstallMultipleProtocolInterfaces (
         ImageHandle,
         &gEfiCallerIdGuid,
         PrivateData,
         NULL
         );

  UninstallTcg2ConfigForm (PrivateData);

  return EFI_SUCCESS;
}
