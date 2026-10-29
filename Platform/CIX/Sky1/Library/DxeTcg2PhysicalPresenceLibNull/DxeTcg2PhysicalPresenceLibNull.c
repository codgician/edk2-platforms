/** @file
  Physical presence interface for platforms without a PP processor.

Copyright 2024 Cix Technology Group Co., Ltd. All Rights Reserved.
SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Library/Tcg2PhysicalPresenceLib.h>

VOID
EFIAPI
Tcg2PhysicalPresenceLibProcessRequest (
  IN TPM2B_AUTH  *PlatformAuth OPTIONAL
  )
{
}

BOOLEAN
EFIAPI
Tcg2PhysicalPresenceLibNeedUserConfirm (
  VOID
  )
{
  return FALSE;
}

UINT32
EFIAPI
Tcg2PhysicalPresenceLibReturnOperationResponseToOsFunction (
  OUT UINT32  *MostRecentRequest,
  OUT UINT32  *Response
  )
{
  // No operation was processed; leave the caller's outputs untouched on error.
  return TCG_PP_RETURN_TPM_OPERATION_RESPONSE_FAILURE;
}

UINT32
Tcg2PhysicalPresenceLibSubmitRequestToPreOSFunctionEx (
  IN OUT UINT32  *OperationRequest,
  IN OUT UINT32  *RequestParameter
  )
{
  return TCG_PP_SUBMIT_REQUEST_TO_PREOS_NOT_IMPLEMENTED;
}

UINT32
EFIAPI
Tcg2PhysicalPresenceLibSubmitRequestToPreOSFunction (
  IN UINT32  OperationRequest,
  IN UINT32  RequestParameter
  )
{
  return TCG_PP_SUBMIT_REQUEST_TO_PREOS_NOT_IMPLEMENTED;
}

UINT32
EFIAPI
Tcg2PhysicalPresenceLibGetUserConfirmationStatusFunction (
  IN UINT32  OperationRequest
  )
{
  return TCG_PP_GET_USER_CONFIRMATION_NOT_IMPLEMENTED;
}

UINT32
EFIAPI
Tcg2PhysicalPresenceLibGetManagementFlags (
  VOID
  )
{
  // Do not advertise RESET_TRACK or storage-management capabilities.
  return TCG2_BIOS_TPM_MANAGEMENT_FLAG_PP_REQUIRED_FOR_CLEAR |
         TCG2_BIOS_TPM_MANAGEMENT_FLAG_PP_REQUIRED_FOR_TURN_ON |
         TCG2_BIOS_TPM_MANAGEMENT_FLAG_PP_REQUIRED_FOR_TURN_OFF |
         TCG2_BIOS_TPM_MANAGEMENT_FLAG_PP_REQUIRED_FOR_CHANGE_EPS |
         TCG2_BIOS_TPM_MANAGEMENT_FLAG_PP_REQUIRED_FOR_CHANGE_PCRS;
}
