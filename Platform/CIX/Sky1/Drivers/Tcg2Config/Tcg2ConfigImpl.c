/** @file
  HII Config Access protocol implementation of TCG2 configuration module.
  NOTE: This module is only for reference only, each platform should have its own setup page.

Copyright 2024 Cix Technology Group Co., Ltd. All Rights Reserved.
Copyright (c) 2015 - 2018, Intel Corporation. All rights reserved.<BR>
(C) Copyright 2018 Hewlett Packard Enterprise Development LP<BR>
SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "Tcg2ConfigImpl.h"
#include <Library/PcdLib.h>
#include <Library/Tpm2CommandLib.h>
#include <Library/Tpm2DeviceLib.h>

#include <Guid/TpmInstance.h>

#include <IndustryStandard/TpmPtp.h>

#define EFI_TCG2_EVENT_LOG_FORMAT_ALL  (EFI_TCG2_EVENT_LOG_FORMAT_TCG_1_2 | EFI_TCG2_EVENT_LOG_FORMAT_TCG_2)


TCG2_CONFIG_PRIVATE_DATA  *mTcg2ConfigPrivateDate;
TCG2_CONFIG_PRIVATE_DATA  mTcg2ConfigPrivateDateTemplate = {
  TCG2_CONFIG_PRIVATE_DATA_SIGNATURE,
  {
    Tcg2ExtractConfig,
    Tcg2RouteConfig,
    Tcg2Callback
  }
};

HII_VENDOR_DEVICE_PATH  mTcg2HiiVendorDevicePath = {
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      {
        (UINT8)(sizeof (VENDOR_DEVICE_PATH)),
        (UINT8)((sizeof (VENDOR_DEVICE_PATH)) >> 8)
      }
    },
    TCG2_CONFIG_FORM_SET_GUID
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    {
      (UINT8)(END_DEVICE_PATH_LENGTH),
      (UINT8)((END_DEVICE_PATH_LENGTH) >> 8)
    }
  }
};

UINT8  mCurrentPpRequest;



/**
  This function allows a caller to extract the current configuration for one
  or more named elements from the target driver.

  @param[in]   This              Points to the EFI_HII_CONFIG_ACCESS_PROTOCOL.
  @param[in]   Request           A null-terminated Unicode string in
                                 <ConfigRequest> format.
  @param[out]  Progress          On return, points to a character in the Request
                                 string. Points to the string's null terminator if
                                 request was successful. Points to the most recent
                                 '&' before the first failing name/value pair (or
                                 the beginning of the string if the failure is in
                                 the first name/value pair) if the request was not
                                 successful.
  @param[out]  Results           A null-terminated Unicode string in
                                 <ConfigAltResp> format which has all values filled
                                 in for the names in the Request string. String to
                                 be allocated by the called function.

  @retval EFI_SUCCESS            The Results is filled with the requested values.
  @retval EFI_OUT_OF_RESOURCES   Not enough memory to store the results.
  @retval EFI_INVALID_PARAMETER  Request is illegal syntax, or unknown name.
  @retval EFI_NOT_FOUND          Routing data doesn't match any storage in this
                                 driver.

**/
EFI_STATUS
EFIAPI
Tcg2ExtractConfig (
  IN CONST EFI_HII_CONFIG_ACCESS_PROTOCOL  *This,
  IN CONST EFI_STRING                      Request,
  OUT EFI_STRING                           *Progress,
  OUT EFI_STRING                           *Results
  )
{
  if ((Progress == NULL) || (Results == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *Progress = Request;
  return EFI_NOT_FOUND;
}

/**
  Save TPM request to variable space.

  @param[in] PpRequest             Physical Presence request command.

  @retval    EFI_SUCCESS           The operation is finished successfully.
  @retval    Others                Other errors as indicated.

**/
EFI_STATUS
SaveTcg2PpRequest (
  IN UINT8  PpRequest
  )
{
  UINT32      ReturnCode;
  EFI_STATUS  Status;
  switch (PpRequest) {
    case TCG2_PHYSICAL_PRESENCE_NO_ACTION:
    case TCG2_PHYSICAL_PRESENCE_CLEAR:
    case TCG2_PHYSICAL_PRESENCE_ENABLE_CLEAR:
    case TCG2_PHYSICAL_PRESENCE_ENABLE_CLEAR_2:
    case TCG2_PHYSICAL_PRESENCE_ENABLE_CLEAR_3:
    case TCG2_PHYSICAL_PRESENCE_CHANGE_EPS:
    case TCG2_PHYSICAL_PRESENCE_LOG_ALL_DIGESTS:
      break;
    case TCG2_PHYSICAL_PRESENCE_SET_PCR_BANKS:
      // Selecting the operation stages it; zero is not a valid bank mask.
      mCurrentPpRequest = PpRequest;
      return EFI_SUCCESS;
    default:
      return EFI_UNSUPPORTED;
  }


  ReturnCode = Tcg2PhysicalPresenceLibSubmitRequestToPreOSFunction (PpRequest, 0);
  if (ReturnCode == TCG_PP_SUBMIT_REQUEST_TO_PREOS_SUCCESS) {
    mCurrentPpRequest = PpRequest;
    Status            = EFI_SUCCESS;
  } else if (ReturnCode == TCG_PP_SUBMIT_REQUEST_TO_PREOS_GENERAL_FAILURE) {
    Status = EFI_OUT_OF_RESOURCES;
  } else if (ReturnCode == TCG_PP_SUBMIT_REQUEST_TO_PREOS_NOT_IMPLEMENTED) {
    Status = EFI_UNSUPPORTED;
  } else {
    Status = EFI_DEVICE_ERROR;
  }

  return Status;
}

/**
  Save TPM request to variable space.

  @param[in] PpRequestParameter    Physical Presence request parameter.

  @retval    EFI_SUCCESS           The operation is finished successfully.
  @retval    Others                Other errors as indicated.

**/
EFI_STATUS
SaveTcg2PpRequestParameter (
  IN UINT32  PpRequestParameter
  )
{
  UINT32      ReturnCode;
  EFI_STATUS  Status;
  if ((mCurrentPpRequest != TCG2_PHYSICAL_PRESENCE_SET_PCR_BANKS) ||
      (PpRequestParameter == 0) || ((PpRequestParameter & ~0x1FU) != 0) ||
      ((PpRequestParameter & ~mTcg2ConfigPrivateDate->ProtocolCapability.HashAlgorithmBitmap) != 0))
  {
    return EFI_INVALID_PARAMETER;
  }


  ReturnCode = Tcg2PhysicalPresenceLibSubmitRequestToPreOSFunction (mCurrentPpRequest, PpRequestParameter);
  if (ReturnCode == TCG_PP_SUBMIT_REQUEST_TO_PREOS_SUCCESS) {
    Status = EFI_SUCCESS;
  } else if (ReturnCode == TCG_PP_SUBMIT_REQUEST_TO_PREOS_GENERAL_FAILURE) {
    Status = EFI_OUT_OF_RESOURCES;
  } else if (ReturnCode == TCG_PP_SUBMIT_REQUEST_TO_PREOS_NOT_IMPLEMENTED) {
    Status = EFI_UNSUPPORTED;
  } else {
    Status = EFI_DEVICE_ERROR;
  }

  return Status;
}

/**
  Save Tcg2 PCR Banks request request to variable space.

  @param[in] PCRBankIndex     PCR Bank Index.
  @param[in] Enable           Enable or disable this PCR Bank.

  @retval    EFI_SUCCESS           The operation is finished successfully.
  @retval    Others                Other errors as indicated.

**/
EFI_STATUS
SaveTcg2PCRBanksRequest (
  IN UINTN    PCRBankIndex,
  IN BOOLEAN  Enable
  )
{
  UINT32      ReturnCode;
  EFI_STATUS  Status;
  UINT32      Desired;

  if ((PCRBankIndex > 4) || (Enable > TRUE)) {
    return EFI_INVALID_PARAMETER;
  }

  Desired = mTcg2ConfigPrivateDate->PCRBanksDesired;

  if (Enable) {
    Desired |= (1U << PCRBankIndex);
  } else {
    Desired &= ~(1U << PCRBankIndex);
  }

  if ((Desired == 0) || ((Desired & ~0x1FU) != 0) ||
      ((Desired & ~mTcg2ConfigPrivateDate->ProtocolCapability.HashAlgorithmBitmap) != 0))
  {
    return EFI_INVALID_PARAMETER;
  }

  ReturnCode = Tcg2PhysicalPresenceLibSubmitRequestToPreOSFunction (TCG2_PHYSICAL_PRESENCE_SET_PCR_BANKS, Desired);
  if (ReturnCode == TCG_PP_SUBMIT_REQUEST_TO_PREOS_SUCCESS) {
    mTcg2ConfigPrivateDate->PCRBanksDesired = Desired;
    Status = EFI_SUCCESS;
  } else if (ReturnCode == TCG_PP_SUBMIT_REQUEST_TO_PREOS_GENERAL_FAILURE) {
    Status = EFI_OUT_OF_RESOURCES;
  } else if (ReturnCode == TCG_PP_SUBMIT_REQUEST_TO_PREOS_NOT_IMPLEMENTED) {
    Status = EFI_UNSUPPORTED;
  } else {
    Status = EFI_DEVICE_ERROR;
  }

  return Status;
}

/**
  This function processes the results of changes in configuration.

  @param[in]  This               Points to the EFI_HII_CONFIG_ACCESS_PROTOCOL.
  @param[in]  Configuration      A null-terminated Unicode string in <ConfigResp>
                                 format.
  @param[out] Progress           A pointer to a string filled in with the offset of
                                 the most recent '&' before the first failing
                                 name/value pair (or the beginning of the string if
                                 the failure is in the first name/value pair) or
                                 the terminating NULL if all was successful.

  @retval EFI_SUCCESS            The Results is processed successfully.
  @retval EFI_INVALID_PARAMETER  Configuration is NULL.
  @retval EFI_NOT_FOUND          Routing data doesn't match any storage in this
                                 driver.

**/
EFI_STATUS
EFIAPI
Tcg2RouteConfig (
  IN CONST EFI_HII_CONFIG_ACCESS_PROTOCOL  *This,
  IN CONST EFI_STRING                      Configuration,
  OUT EFI_STRING                           *Progress
  )
{
  if ((Configuration == NULL) || (Progress == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *Progress = Configuration;

  return EFI_NOT_FOUND;
}



/**
  This function processes the results of changes in configuration.

  @param[in]  This               Points to the EFI_HII_CONFIG_ACCESS_PROTOCOL.
  @param[in]  Action             Specifies the type of action taken by the browser.
  @param[in]  QuestionId         A unique value which is sent to the original
                                 exporting driver so that it can identify the type
                                 of data to expect.
  @param[in]  Type               The type of value for the question.
  @param[in]  Value              A pointer to the data being sent to the original
                                 exporting driver.
  @param[out] ActionRequest      On return, points to the action requested by the
                                 callback function.

  @retval EFI_SUCCESS            The callback successfully handled the action.
  @retval EFI_OUT_OF_RESOURCES   Not enough storage is available to hold the
                                 variable and its data.
  @retval EFI_DEVICE_ERROR       The variable could not be saved.
  @retval EFI_UNSUPPORTED        The specified Action is not supported by the
                                 callback.

**/
EFI_STATUS
EFIAPI
Tcg2Callback (
  IN CONST EFI_HII_CONFIG_ACCESS_PROTOCOL  *This,
  IN     EFI_BROWSER_ACTION                Action,
  IN     EFI_QUESTION_ID                   QuestionId,
  IN     UINT8                             Type,
  IN     EFI_IFR_TYPE_VALUE                *Value,
  OUT EFI_BROWSER_ACTION_REQUEST           *ActionRequest
  )
{
  if ((This == NULL) || (Value == NULL) || (ActionRequest == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *ActionRequest = EFI_BROWSER_ACTION_REQUEST_NONE;
  // Neither browser writes nor CHANGING callbacks may switch the TPM backend,
  // interface or advertised security contract. Only queued PP requests remain.
  if (Action != EFI_BROWSER_ACTION_CHANGED) {
    return EFI_UNSUPPORTED;
  }

  if (QuestionId == KEY_TPM2_OPERATION) {
    if (Type != EFI_IFR_TYPE_NUM_SIZE_8) {
      return EFI_INVALID_PARAMETER;
    }

    return SaveTcg2PpRequest (Value->u8);
  }

  if (QuestionId == KEY_TPM2_OPERATION_PARAMETER) {
    if (Type != EFI_IFR_TYPE_NUM_SIZE_32) {
      return EFI_INVALID_PARAMETER;
    }

    return SaveTcg2PpRequestParameter (Value->u32);
  }

  if ((QuestionId >= KEY_TPM2_PCR_BANKS_REQUEST_0) && (QuestionId <= KEY_TPM2_PCR_BANKS_REQUEST_4)) {
    if ((Type != EFI_IFR_TYPE_BOOLEAN) || (Value->b > TRUE)) {
      return EFI_INVALID_PARAMETER;
    }

    return SaveTcg2PCRBanksRequest (QuestionId - KEY_TPM2_PCR_BANKS_REQUEST_0, Value->b);
  }

  return EFI_UNSUPPORTED;
}

/**
  Append Buffer With TpmAlgHash.

  @param[in] Buffer               Buffer to be appended.
  @param[in] BufferSize           Size of buffer.
  @param[in] TpmAlgHash           TpmAlgHash.

**/
VOID
AppendBufferWithTpmAlgHash (
  IN UINT16  *Buffer,
  IN UINTN   BufferSize,
  IN UINT32  TpmAlgHash
  )
{
  switch (TpmAlgHash) {
    case TPM_ALG_SHA1:
      if (Buffer[0] != 0) {
        StrCatS (Buffer, BufferSize / sizeof (CHAR16), L", ");
      }

      StrCatS (Buffer, BufferSize / sizeof (CHAR16), L"SHA1");
      break;
    case TPM_ALG_SHA256:
      if (Buffer[0] != 0) {
        StrCatS (Buffer, BufferSize / sizeof (CHAR16), L", ");
      }

      StrCatS (Buffer, BufferSize / sizeof (CHAR16), L"SHA256");
      break;
    case TPM_ALG_SHA384:
      if (Buffer[0] != 0) {
        StrCatS (Buffer, BufferSize / sizeof (CHAR16), L", ");
      }

      StrCatS (Buffer, BufferSize / sizeof (CHAR16), L"SHA384");
      break;
    case TPM_ALG_SHA512:
      if (Buffer[0] != 0) {
        StrCatS (Buffer, BufferSize / sizeof (CHAR16), L", ");
      }

      StrCatS (Buffer, BufferSize / sizeof (CHAR16), L"SHA512");
      break;
    case TPM_ALG_SM3_256:
      if (Buffer[0] != 0) {
        StrCatS (Buffer, BufferSize / sizeof (CHAR16), L", ");
      }

      StrCatS (Buffer, BufferSize / sizeof (CHAR16), L"SM3_256");
      break;
  }
}

/**
  Fill Buffer With BootHashAlg.

  @param[in] Buffer               Buffer to be filled.
  @param[in] BufferSize           Size of buffer.
  @param[in] BootHashAlg          BootHashAlg.

**/
VOID
FillBufferWithBootHashAlg (
  IN UINT16  *Buffer,
  IN UINTN   BufferSize,
  IN UINT32  BootHashAlg
  )
{
  Buffer[0] = 0;
  if ((BootHashAlg & EFI_TCG2_BOOT_HASH_ALG_SHA1) != 0) {
    if (Buffer[0] != 0) {
      StrCatS (Buffer, BufferSize / sizeof (CHAR16), L", ");
    }

    StrCatS (Buffer, BufferSize / sizeof (CHAR16), L"SHA1");
  }

  if ((BootHashAlg & EFI_TCG2_BOOT_HASH_ALG_SHA256) != 0) {
    if (Buffer[0] != 0) {
      StrCatS (Buffer, BufferSize / sizeof (CHAR16), L", ");
    }

    StrCatS (Buffer, BufferSize / sizeof (CHAR16), L"SHA256");
  }

  if ((BootHashAlg & EFI_TCG2_BOOT_HASH_ALG_SHA384) != 0) {
    if (Buffer[0] != 0) {
      StrCatS (Buffer, BufferSize / sizeof (CHAR16), L", ");
    }

    StrCatS (Buffer, BufferSize / sizeof (CHAR16), L"SHA384");
  }

  if ((BootHashAlg & EFI_TCG2_BOOT_HASH_ALG_SHA512) != 0) {
    if (Buffer[0] != 0) {
      StrCatS (Buffer, BufferSize / sizeof (CHAR16), L", ");
    }

    StrCatS (Buffer, BufferSize / sizeof (CHAR16), L"SHA512");
  }

  if ((BootHashAlg & EFI_TCG2_BOOT_HASH_ALG_SM3_256) != 0) {
    if (Buffer[0] != 0) {
      StrCatS (Buffer, BufferSize / sizeof (CHAR16), L", ");
    }

    StrCatS (Buffer, BufferSize / sizeof (CHAR16), L"SM3_256");
  }
}

/**
  Set ConfigInfo according to TpmAlgHash.

  @param[in,out] Tcg2ConfigInfo       TCG2 config info.
  @param[in]     TpmAlgHash           TpmAlgHash.

**/
VOID
SetConfigInfo (
  IN OUT TCG2_CONFIGURATION_INFO  *Tcg2ConfigInfo,
  IN UINT32                       TpmAlgHash
  )
{
  switch (TpmAlgHash) {
    case TPM_ALG_SHA1:
      Tcg2ConfigInfo->Sha1Supported = TRUE;
      break;
    case TPM_ALG_SHA256:
      Tcg2ConfigInfo->Sha256Supported = TRUE;
      break;
    case TPM_ALG_SHA384:
      Tcg2ConfigInfo->Sha384Supported = TRUE;
      break;
    case TPM_ALG_SHA512:
      Tcg2ConfigInfo->Sha512Supported = TRUE;
      break;
    case TPM_ALG_SM3_256:
      Tcg2ConfigInfo->Sm3Supported = TRUE;
      break;
  }
}

/**
  Fill Buffer With TCG2EventLogFormat.

  @param[in] Buffer               Buffer to be filled.
  @param[in] BufferSize           Size of buffer.
  @param[in] TCG2EventLogFormat   TCG2EventLogFormat.

**/
VOID
FillBufferWithTCG2EventLogFormat (
  IN UINT16  *Buffer,
  IN UINTN   BufferSize,
  IN UINT32  TCG2EventLogFormat
  )
{
  Buffer[0] = 0;
  if ((TCG2EventLogFormat & EFI_TCG2_EVENT_LOG_FORMAT_TCG_1_2) != 0) {
    if (Buffer[0] != 0) {
      StrCatS (Buffer, BufferSize / sizeof (CHAR16), L", ");
    }

    StrCatS (Buffer, BufferSize / sizeof (CHAR16), L"TCG_1_2");
  }

  if ((TCG2EventLogFormat & EFI_TCG2_EVENT_LOG_FORMAT_TCG_2) != 0) {
    if (Buffer[0] != 0) {
      StrCatS (Buffer, BufferSize / sizeof (CHAR16), L", ");
    }

    StrCatS (Buffer, BufferSize / sizeof (CHAR16), L"TCG_2");
  }

  if ((TCG2EventLogFormat & (~EFI_TCG2_EVENT_LOG_FORMAT_ALL)) != 0) {
    if (Buffer[0] != 0) {
      StrCatS (Buffer, BufferSize / sizeof (CHAR16), L", ");
    }

    StrCatS (Buffer, BufferSize / sizeof (CHAR16), L"UNKNOWN");
  }
}

/**
  This function publish the TCG2 configuration Form for TPM device.

  @param[in, out]  PrivateData   Points to TCG2 configuration private data.

  @retval EFI_SUCCESS            HII Form is installed for this network device.
  @retval EFI_OUT_OF_RESOURCES   Not enough resource for HII Form installation.
  @retval Others                 Other errors as indicated.

**/
EFI_STATUS
InstallTcg2ConfigForm (
  IN OUT TCG2_CONFIG_PRIVATE_DATA  *PrivateData
  )
{
  EFI_STATUS                      Status;
  EFI_HII_HANDLE                  HiiHandle;
  EFI_HANDLE                      DriverHandle;
  EFI_HII_CONFIG_ACCESS_PROTOCOL  *ConfigAccess;
  UINTN                           Index;
  TPML_PCR_SELECTION              Pcrs;
  CHAR16                          TempBuffer[1024];
  TCG2_CONFIGURATION_INFO         Tcg2ConfigInfo;
  BOOLEAN                         IsCmdImp = FALSE;

  DriverHandle = NULL;
  ConfigAccess = &PrivateData->ConfigAccess;
  Status       = gBS->InstallMultipleProtocolInterfaces (
                        &DriverHandle,
                        &gEfiDevicePathProtocolGuid,
                        &mTcg2HiiVendorDevicePath,
                        &gEfiHiiConfigAccessProtocolGuid,
                        ConfigAccess,
                        NULL
                        );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  PrivateData->DriverHandle = DriverHandle;

  //
  // Publish the HII package list
  //
  HiiHandle = HiiAddPackages (
                &gTcg2ConfigFormSetGuid,
                DriverHandle,
                Tcg2ConfigDxeStrings,
                Tcg2ConfigBin,
                NULL
                );
  if (HiiHandle == NULL) {
    gBS->UninstallMultipleProtocolInterfaces (
           DriverHandle,
           &gEfiDevicePathProtocolGuid,
           &mTcg2HiiVendorDevicePath,
           &gEfiHiiConfigAccessProtocolGuid,
           ConfigAccess,
           NULL
           );

    return EFI_OUT_OF_RESOURCES;
  }

  PrivateData->HiiHandle = HiiHandle;

  //
  // Update static data
  //
  switch (PrivateData->TpmDeviceDetected) {
    case TPM_DEVICE_NULL:
      HiiSetString (PrivateData->HiiHandle, STRING_TOKEN (STR_TCG2_DEVICE_STATE_CONTENT), L"Not Found", NULL);
      break;
    case TPM_DEVICE_1_2:
      HiiSetString (PrivateData->HiiHandle, STRING_TOKEN (STR_TCG2_DEVICE_STATE_CONTENT), L"TPM 1.2", NULL);
      break;
    case TPM_DEVICE_2_0_DTPM:
      HiiSetString (PrivateData->HiiHandle, STRING_TOKEN (STR_TCG2_DEVICE_STATE_CONTENT), L"TPM 2.0", NULL);
      break;
    default:
      HiiSetString (PrivateData->HiiHandle, STRING_TOKEN (STR_TCG2_DEVICE_STATE_CONTENT), L"Unknown", NULL);
      break;
  }

  ZeroMem (&Tcg2ConfigInfo, sizeof (Tcg2ConfigInfo));
  Status = Tpm2GetCapabilityPcrs (&Pcrs);
  if (EFI_ERROR (Status)) {
    HiiSetString (PrivateData->HiiHandle, STRING_TOKEN (STR_TPM2_ACTIVE_HASH_ALGO_CONTENT), L"[Unknown]", NULL);
    HiiSetString (PrivateData->HiiHandle, STRING_TOKEN (STR_TPM2_SUPPORTED_HASH_ALGO_CONTENT), L"[Unknown]", NULL);
  } else {
    TempBuffer[0] = 0;
    for (Index = 0; Index < Pcrs.count; Index++) {
      if (!IsZeroBuffer (Pcrs.pcrSelections[Index].pcrSelect, Pcrs.pcrSelections[Index].sizeofSelect)) {
        AppendBufferWithTpmAlgHash (TempBuffer, sizeof (TempBuffer), Pcrs.pcrSelections[Index].hash);
      }
    }

    HiiSetString (PrivateData->HiiHandle, STRING_TOKEN (STR_TPM2_ACTIVE_HASH_ALGO_CONTENT), TempBuffer, NULL);

    TempBuffer[0] = 0;
    for (Index = 0; Index < Pcrs.count; Index++) {
      AppendBufferWithTpmAlgHash (TempBuffer, sizeof (TempBuffer), Pcrs.pcrSelections[Index].hash);
      SetConfigInfo (&Tcg2ConfigInfo, Pcrs.pcrSelections[Index].hash);
    }

    HiiSetString (PrivateData->HiiHandle, STRING_TOKEN (STR_TPM2_SUPPORTED_HASH_ALGO_CONTENT), TempBuffer, NULL);
  }

  Status = Tpm2GetCapabilityIsCommandImplemented (TPM_CC_ChangeEPS, &IsCmdImp);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Tpm2GetCapabilityIsCmdImpl fails %r\n", Status));
  }

  Tcg2ConfigInfo.ChangeEPSSupported = IsCmdImp;

  FillBufferWithBootHashAlg (TempBuffer, sizeof (TempBuffer), PcdGet32 (PcdTcg2HashAlgorithmBitmap));
  HiiSetString (PrivateData->HiiHandle, STRING_TOKEN (STR_BIOS_HASH_ALGO_CONTENT), TempBuffer, NULL);

  //
  // Tcg2 Capability
  //
  FillBufferWithTCG2EventLogFormat (TempBuffer, sizeof (TempBuffer), PrivateData->ProtocolCapability.SupportedEventLogs);
  HiiSetString (PrivateData->HiiHandle, STRING_TOKEN (STR_TCG2_SUPPORTED_EVENT_LOG_FORMAT_CONTENT), TempBuffer, NULL);

  FillBufferWithBootHashAlg (TempBuffer, sizeof (TempBuffer), PrivateData->ProtocolCapability.HashAlgorithmBitmap);
  HiiSetString (PrivateData->HiiHandle, STRING_TOKEN (STR_TCG2_HASH_ALGO_BITMAP_CONTENT), TempBuffer, NULL);

  UnicodeSPrint (TempBuffer, sizeof (TempBuffer), L"%d", PrivateData->ProtocolCapability.NumberOfPCRBanks);
  HiiSetString (PrivateData->HiiHandle, STRING_TOKEN (STR_TCG2_NUMBER_OF_PCR_BANKS_CONTENT), TempBuffer, NULL);

  FillBufferWithBootHashAlg (TempBuffer, sizeof (TempBuffer), PrivateData->ProtocolCapability.ActivePcrBanks);
  HiiSetString (PrivateData->HiiHandle, STRING_TOKEN (STR_TCG2_ACTIVE_PCR_BANKS_CONTENT), TempBuffer, NULL);
  // The fTPM transport has no writable PTP interface selection.
  HiiSetString (PrivateData->HiiHandle, STRING_TOKEN (STR_TCG2_DEVICE_INTERFACE_STATE_CONTENT), L"Firmware TPM", NULL);
  HiiSetString (PrivateData->HiiHandle, STRING_TOKEN (STR_TCG2_DEVICE_INTERFACE_CAPABILITY_CONTENT), L"Fixed", NULL);

  //
  // Set ConfigInfo, to control the check box.
  //
  Status = gRT->SetVariable (
                  TCG2_STORAGE_INFO_NAME,
                  &gTcg2ConfigFormSetGuid,
                  EFI_VARIABLE_BOOTSERVICE_ACCESS,
                  sizeof (Tcg2ConfigInfo),
                  &Tcg2ConfigInfo
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Tcg2ConfigDriver: Fail to set TCG2_STORAGE_INFO_NAME\n"));
  }

  return EFI_SUCCESS;
}

/**
  This function removes TCG2 configuration Form.

  @param[in, out]  PrivateData   Points to TCG2 configuration private data.

**/
VOID
UninstallTcg2ConfigForm (
  IN OUT TCG2_CONFIG_PRIVATE_DATA  *PrivateData
  )
{
  //
  // Uninstall HII package list
  //
  if (PrivateData->HiiHandle != NULL) {
    HiiRemovePackages (PrivateData->HiiHandle);
    PrivateData->HiiHandle = NULL;
  }

  //
  // Uninstall HII Config Access Protocol
  //
  if (PrivateData->DriverHandle != NULL) {
    gBS->UninstallMultipleProtocolInterfaces (
           PrivateData->DriverHandle,
           &gEfiDevicePathProtocolGuid,
           &mTcg2HiiVendorDevicePath,
           &gEfiHiiConfigAccessProtocolGuid,
           &PrivateData->ConfigAccess,
           NULL
           );
    PrivateData->DriverHandle = NULL;
  }

  FreePool (PrivateData);
}
