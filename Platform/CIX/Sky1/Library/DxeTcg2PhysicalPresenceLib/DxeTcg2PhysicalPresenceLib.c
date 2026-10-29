/** @file
  Validate OS physical-presence requests and execute only console-authorized changes.

  Copyright 2024 Cix Technology Group Co., Ltd. All Rights Reserved.
  Copyright (c) 2013 - 2020, Intel Corporation. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>
#include <Library/PcdLib.h>
#include <Library/HobLib.h>
#include <Library/Tpm2CommandLib.h>
#include <Library/Tcg2PhysicalPresenceLib.h>
#include <Library/UefiLib.h>
#include "PpPolicy.h"

#define PP_PCR_BANK_MASK    (EFI_TCG2_BOOT_HASH_ALG_SHA1 | EFI_TCG2_BOOT_HASH_ALG_SHA256 | EFI_TCG2_BOOT_HASH_ALG_SHA384 | EFI_TCG2_BOOT_HASH_ALG_SHA512 | EFI_TCG2_BOOT_HASH_ALG_SM3_256)
#define PP_CONFIRM_TIMEOUT  (30 * 10000000ULL)

// The ACPI table and firmware share this fixed, platform-reserved mailbox.
typedef struct {
  UINT32    Ppio;
  UINT32    Ppir;
  UINT32    Ppis;
} TPMC;

/** Recognize only operations actually implemented by this platform.

  Optional flag-waiver, hierarchy control, storage and vendor operations are not
  implemented. In particular no persisted flag can waive console authorization.

  @param[in] Operation  Full-width PPI operation, before any narrowing.
  @retval TRUE         The operation is implemented.
**/
STATIC
BOOLEAN
PpKnownOperation (
  IN UINT32  Operation
  )
{
  switch (Operation) {
    case TCG2_PHYSICAL_PRESENCE_SET_PCR_BANKS:
      return TRUE;
    case TCG2_PHYSICAL_PRESENCE_NO_ACTION:
    case TCG2_PHYSICAL_PRESENCE_CLEAR:
    case TCG2_PHYSICAL_PRESENCE_ENABLE_CLEAR:
    case TCG2_PHYSICAL_PRESENCE_ENABLE_CLEAR_2:
    case TCG2_PHYSICAL_PRESENCE_ENABLE_CLEAR_3:
    case TCG2_PHYSICAL_PRESENCE_CHANGE_EPS:
    case TCG2_PHYSICAL_PRESENCE_LOG_ALL_DIGESTS:
      return TRUE;
    default:
      return FALSE;
  }
}

/** Validate an implemented operation's parameter domain.

  @param[in] Operation  Full-width PPI operation.
  @param[in] Parameter Nonzero known PCR bitmap for SET_PCR_BANKS; zero otherwise.
  @retval TRUE         The complete request is valid.
**/
STATIC
BOOLEAN
PpValidOperation (
  IN UINT32  Operation,
  IN UINT32  Parameter
  )
{
  if (!PpKnownOperation (Operation)) {
    return FALSE;
  }

  return (Operation == TCG2_PHYSICAL_PRESENCE_SET_PCR_BANKS) ?
         ((Parameter != 0) && ((Parameter & ~PP_PCR_BANK_MASK) == 0)) :
         (Parameter == 0);
}

/** Read untrusted request storage; partial successful reads are never valid.

  @param[out] Request  Validated snapshot, or zeroes on failure.
  @retval EFI_SUCCESS Complete request and response schemas are valid.
  @retval Others      Missing, unreadable or malformed storage.
**/
STATIC
EFI_STATUS
PpReadRequest (
  OUT EFI_TCG2_PHYSICAL_PRESENCE  *Request
  )
{
  EFI_STATUS  Status;
  UINTN       Size;
  UINT32      Attributes;

  ZeroMem (Request, sizeof (*Request));
  Attributes = 0;
  Size       = sizeof (*Request);
  Status     = gRT->GetVariable (
                                 TCG2_PHYSICAL_PRESENCE_VARIABLE,
                                 &gEfiTcg2PhysicalPresenceGuid,
                                 &Attributes,
                                 &Size,
                                 Request
                                 );
  if (!EFI_ERROR (Status) &&
      ((Size != sizeof (*Request)) || (Attributes != PP_VARIABLE_ATTRIBUTES) ||
       !PpValidOperation (Request->PPRequest, Request->PPRequestParameter) ||
       !PpKnownOperation (Request->LastPPRequest) ||
       ((Request->PPResponse != TCG_PP_OPERATION_RESPONSE_SUCCESS) &&
        (Request->PPResponse != TCG_PP_OPERATION_RESPONSE_USER_ABORT) &&
        (Request->PPResponse != TCG_PP_OPERATION_RESPONSE_BIOS_FAILURE))))
  {
    Status = EFI_COMPROMISED_DATA;
  }

  if (EFI_ERROR (Status)) {
    ZeroMem (Request, sizeof (*Request));
    DEBUG ((DEBUG_ERROR, "[TPM2] Invalid PP request: %r\n", Status));
  }

  return Status;
}

/** Persist the consumed request before any mutating TPM command.

  @param[in] Request  Complete, validated request/result snapshot.
  @return            The checked variable-service result.
**/
STATIC
EFI_STATUS
PpWriteRequest (
  IN EFI_TCG2_PHYSICAL_PRESENCE  *Request
  )
{
  EFI_STATUS  Status;

  Status = gRT->SetVariable (
                             TCG2_PHYSICAL_PRESENCE_VARIABLE,
                             &gEfiTcg2PhysicalPresenceGuid,
                             PP_VARIABLE_ATTRIBUTES,
                             sizeof (*Request),
                             Request
                             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[TPM2] Cannot save PP result: %r\n", Status));
  }

  return Status;
}

/** Resolve and validate PCR allocations before asking for authorization.

  @param[in] Operation   Validated PPI operation.
  @param[in] Parameter   Validated operation parameter.
  @param[out] Supported  TPM-supported known bank bitmap.
  @param[out] Desired    Allocation the operator would authorize.
  @param[out] NoChange   TRUE only when the allocation already matches.
  @return               EFI_SUCCESS or the capability/schema error.
**/
STATIC
EFI_STATUS
PpGetAllocation (
  IN  UINT32   Operation,
  IN  UINT32   Parameter,
  OUT UINT32   *Supported,
  OUT UINT32   *Desired,
  OUT BOOLEAN  *NoChange
  )
{
  EFI_STATUS  Status;
  UINT32      Active;

  *NoChange  = FALSE;
  *Supported = 0;
  *Desired   = 0;
  if ((Operation != TCG2_PHYSICAL_PRESENCE_SET_PCR_BANKS) &&
      (Operation != TCG2_PHYSICAL_PRESENCE_LOG_ALL_DIGESTS))
  {
    return EFI_SUCCESS;
  }

  Active = 0;
  Status = Tpm2GetCapabilitySupportedAndActivePcrs (Supported, &Active);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *Desired = (Operation == TCG2_PHYSICAL_PRESENCE_LOG_ALL_DIGESTS) ? *Supported : Parameter;
  if ((*Supported == 0) || ((*Supported & ~PP_PCR_BANK_MASK) != 0) ||
      (Active == 0) || ((Active & ~*Supported) != 0) ||
      (*Desired == 0) || ((*Desired & ~*Supported) != 0))
  {
    return EFI_COMPROMISED_DATA;
  }

  *NoChange = *Desired == Active;
  return EFI_SUCCESS;
}

/** Ask the present console operator; errors, ESC and timeout reject the request.

  Reset the input queue before displaying the prompt so buffered keystrokes
  cannot authorize a request the operator has not yet seen.

  @param[in] Operation  Validated operation displayed to the operator.
  @param[in] Desired    Resolved PCR bank bitmap for allocation requests.
  @retval TRUE         F12 was received after a successfully displayed prompt.
  @retval FALSE        Rejected, timed out, or console/timer service failed.
**/
STATIC
BOOLEAN
PpConfirm (
  IN UINT32  Operation,
  IN UINT32  Desired
  )
{
  EFI_STATUS     Status;
  EFI_STATUS     CloseStatus;
  EFI_EVENT      Timer;
  EFI_EVENT      Events[2];
  EFI_INPUT_KEY  Key;
  UINTN          Index;
  BOOLEAN        Accepted;
  UINTN          Printed;

  if ((gST == NULL) || (gST->ConIn == NULL) || (gST->ConOut == NULL)) {
    return FALSE;
  }

  Status = gST->ConIn->Reset (gST->ConIn, FALSE);
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  Status = gBS->CreateEvent (EVT_TIMER, TPL_APPLICATION, NULL, NULL, &Timer);
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  Accepted = FALSE;
  Status   = gBS->SetTimer (Timer, TimerRelative, PP_CONFIRM_TIMEOUT);
  if (!EFI_ERROR (Status)) {
    Printed = Print (L"\nTPM physical-presence request %u\n", Operation);
    if (Printed == 0) {
      goto Done;
    }

    if ((Operation == TCG2_PHYSICAL_PRESENCE_SET_PCR_BANKS) ||
        (Operation == TCG2_PHYSICAL_PRESENCE_LOG_ALL_DIGESTS))
    {
      Printed = Print (L"Change active PCR banks to 0x%x. This can prevent access to PCR-bound secrets.\n", Desired);
    } else if (Operation == TCG2_PHYSICAL_PRESENCE_CHANGE_EPS) {
      Printed = Print (L"Change the TPM endorsement identity. Existing keys and encrypted data may become inaccessible.\n");
    } else {
      Printed = Print (L"Clear the TPM. Existing keys and encrypted data will be lost.\n");
    }

    if ((Printed == 0) ||
        (Print (L"Press F12 to authorize, or ESC to reject. No response within 30 seconds rejects the request.\n") == 0))
    {
      goto Done;
    }

    Events[0] = gST->ConIn->WaitForKey;
    Events[1] = Timer;
    while (TRUE) {
      // A stream of unrelated keys must not postpone expiration indefinitely.
      if (gBS->CheckEvent (Timer) != EFI_NOT_READY) {
        break;
      }

      Status = gBS->WaitForEvent (ARRAY_SIZE (Events), Events, &Index);
      if (EFI_ERROR (Status) || (Index != 0)) {
        break;
      }

      ZeroMem (&Key, sizeof (Key));
      Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
      if (EFI_ERROR (Status)) {
        break;
      }

      if (Key.ScanCode == SCAN_ESC) {
        break;
      }

      if (Key.ScanCode == SCAN_F12) {
        Accepted = TRUE;
        break;
      }
    }
  }

Done:
  CloseStatus = gBS->CloseEvent (Timer);
  return Accepted && !EFI_ERROR (CloseStatus);
}

/** Execute exactly the authorized operation; erase the local auth copy.

  @param[in] PlatformAuth  Optional platform authorization.
  @param[in] Operation     Validated and freshly confirmed operation.
  @param[in] Supported     TPM-supported PCR bank bitmap.
  @param[in] Desired       Confirmed PCR allocation.
  @return                 The first failing TPM result, or EFI_SUCCESS.
**/
STATIC
EFI_STATUS
PpExecute (
  IN TPM2B_AUTH  *PlatformAuth OPTIONAL,
  IN UINT32      Operation,
  IN UINT32      Supported,
  IN UINT32      Desired
  )
{
  EFI_STATUS         Status;
  TPMS_AUTH_COMMAND  Session;
  TPMS_AUTH_COMMAND  *Auth;
  volatile UINT8     *Erase;

  ZeroMem (&Session, sizeof (Session));
  Auth = NULL;
  if (PlatformAuth != NULL) {
    if (PlatformAuth->size > sizeof (Session.hmac.buffer)) {
      return EFI_INVALID_PARAMETER;
    }

    Session.sessionHandle = TPM_RS_PW;
    Session.hmac.size     = PlatformAuth->size;
    CopyMem (Session.hmac.buffer, PlatformAuth->buffer, PlatformAuth->size);
    Auth = &Session;
  }

  switch (Operation) {
    case TCG2_PHYSICAL_PRESENCE_CLEAR:
    case TCG2_PHYSICAL_PRESENCE_ENABLE_CLEAR:
    case TCG2_PHYSICAL_PRESENCE_ENABLE_CLEAR_2:
    case TCG2_PHYSICAL_PRESENCE_ENABLE_CLEAR_3:
      Status = Tpm2ClearControl (TPM_RH_PLATFORM, Auth, NO);
      if (!EFI_ERROR (Status)) {
        Status = Tpm2Clear (TPM_RH_PLATFORM, Auth);
      }

      break;
    case TCG2_PHYSICAL_PRESENCE_CHANGE_EPS:
      Status = Tpm2ChangeEPS (TPM_RH_PLATFORM, Auth);
      break;
    case TCG2_PHYSICAL_PRESENCE_SET_PCR_BANKS:
    case TCG2_PHYSICAL_PRESENCE_LOG_ALL_DIGESTS:
      Status = Tpm2PcrAllocateBanks (PlatformAuth, Supported, Desired);
      break;
    default:
      Status = EFI_UNSUPPORTED;
      break;
  }

  // Volatile writes prevent the compiler from eliminating secret erasure.
  Erase = (volatile UINT8 *)&Session;
  while (Erase < (volatile UINT8 *)(&Session + 1)) {
    *Erase++ = 0;
  }

  return Status;
}

/** Execute a pending request only after schema, policy and console checks.

  @param[in] PlatformAuth  Optional platform authorization; NULL is supported.
**/
VOID
EFIAPI
Tcg2PhysicalPresenceLibProcessRequest (
  IN TPM2B_AUTH  *PlatformAuth OPTIONAL
  )
{
  EFI_STATUS                        Status;
  EFI_TCG2_PHYSICAL_PRESENCE        Request;
  EFI_TCG2_PHYSICAL_PRESENCE_FLAGS  Flags;
  UINT32                            Operation;
  UINT32                            Supported;
  UINT32                            Desired;
  BOOLEAN                           NoChange;
  BOOLEAN                           Accepted;
  TPMC                              *Mailbox;

  if (GetBootModeHob () == BOOT_ON_S4_RESUME) {
    return;
  }

  Status = PpVerifyFlagsLock ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[TPM2] PP flags lock unavailable: %r; skipping requests\n", Status));
    return;
  }

  if (EFI_ERROR (PpReadFlags (&Flags))) {
    return;
  }

  Status = PpReadRequest (&Request);
  if (Status == EFI_NOT_FOUND) {
    // Only absence permits initial creation; corrupt storage is left untouched.
    PpWriteRequest (&Request);
    return;
  }

  if (EFI_ERROR (Status) || (Request.PPRequest == TCG2_PHYSICAL_PRESENCE_NO_ACTION)) {
    return;
  }

  Operation = Request.PPRequest;
  Status    = PpGetAllocation (Operation, Request.PPRequestParameter, &Supported, &Desired, &NoChange);
  Accepted  = FALSE;
  if (!EFI_ERROR (Status) && !NoChange) {
    Accepted = PpConfirm (Operation, Desired);
  }

  Request.LastPPRequest      = Request.PPRequest;
  Request.PPRequest          = TCG2_PHYSICAL_PRESENCE_NO_ACTION;
  Request.PPRequestParameter = 0;
  Request.PPResponse         = EFI_ERROR (Status) ? TCG_PP_OPERATION_RESPONSE_BIOS_FAILURE :
                               (NoChange ? TCG_PP_OPERATION_RESPONSE_SUCCESS : TCG_PP_OPERATION_RESPONSE_USER_ABORT);
  if (Accepted) {
    // If execution or the final result write fails, never replay the mutation.
    Request.PPResponse = TCG_PP_OPERATION_RESPONSE_BIOS_FAILURE;
  }

  if (EFI_ERROR (PpWriteRequest (&Request))) {
    return;
  }

  if (Accepted) {
    Status             = PpExecute (PlatformAuth, Operation, Supported, Desired);
    Request.PPResponse = EFI_ERROR (Status) ? TCG_PP_OPERATION_RESPONSE_BIOS_FAILURE : TCG_PP_OPERATION_RESPONSE_SUCCESS;
    if (EFI_ERROR (PpWriteRequest (&Request))) {
      return;
    }
  }

  Mailbox       = (TPMC *)(UINTN)FixedPcdGet32 (PcdTPMPPIMemoryBase);
  Mailbox->Ppio = 0;
  Mailbox->Ppir = Request.LastPPRequest;
  Mailbox->Ppis = Request.PPResponse;
  if (Accepted && !EFI_ERROR (Status)) {
    gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
  }
}

/** Invalid or unprotected input must not drive confirmation UI.

  @retval TRUE   A valid state-changing request requires console confirmation.
  @retval FALSE  No action, matching allocation, invalid schema or missing lock.
**/
BOOLEAN
EFIAPI
Tcg2PhysicalPresenceLibNeedUserConfirm (
  VOID
  )
{
  EFI_TCG2_PHYSICAL_PRESENCE        Request;
  EFI_TCG2_PHYSICAL_PRESENCE_FLAGS  Flags;
  UINT32                            Supported;
  UINT32                            Desired;
  BOOLEAN                           NoChange;

  if ((GetBootModeHob () == BOOT_ON_S4_RESUME) ||
      EFI_ERROR (PpVerifyFlagsLock ()) || EFI_ERROR (PpReadFlags (&Flags)) ||
      EFI_ERROR (PpReadRequest (&Request)) ||
      (Request.PPRequest == TCG2_PHYSICAL_PRESENCE_NO_ACTION) ||
      EFI_ERROR (PpGetAllocation (Request.PPRequest, Request.PPRequestParameter, &Supported, &Desired, &NoChange)))
  {
    return FALSE;
  }

  return !NoChange;
}

/** Return only validated response fields; malformed storage returns no data.

  @param[out] MostRecentRequest  Last completed operation, or zero on failure.
  @param[out] Response           Last result, or zero on failure.
  @return                       PPI response-transport success/failure code.
**/
UINT32
EFIAPI
Tcg2PhysicalPresenceLibReturnOperationResponseToOsFunction (
  OUT UINT32  *MostRecentRequest,
  OUT UINT32  *Response
  )
{
  EFI_TCG2_PHYSICAL_PRESENCE  Request;

  if ((MostRecentRequest == NULL) || (Response == NULL)) {
    return TCG_PP_RETURN_TPM_OPERATION_RESPONSE_FAILURE;
  }

  *MostRecentRequest = 0;
  *Response          = 0;
  if (EFI_ERROR (PpReadRequest (&Request))) {
    return TCG_PP_RETURN_TPM_OPERATION_RESPONSE_FAILURE;
  }

  *MostRecentRequest = Request.LastPPRequest;
  *Response          = Request.PPResponse;
  return TCG_PP_RETURN_TPM_OPERATION_RESPONSE_SUCCESS;
}

/** Queue a complete implemented request, never a narrowed/truncated opcode.

  @param[in] OperationRequest  Full-width operation code.
  @param[in] RequestParameter Validated parameter for that operation.
  @return                    PPI submission status; success is not authorization.
**/
UINT32
EFIAPI
Tcg2PhysicalPresenceLibSubmitRequestToPreOSFunction (
  IN UINT32  OperationRequest,
  IN UINT32  RequestParameter
  )
{
  EFI_TCG2_PHYSICAL_PRESENCE        Request;
  EFI_TCG2_PHYSICAL_PRESENCE_FLAGS  Flags;

  if (!PpValidOperation (OperationRequest, RequestParameter)) {
    return TCG_PP_SUBMIT_REQUEST_TO_PREOS_NOT_IMPLEMENTED;
  }

  // Queuing is not authorization. The pre-OS executor verifies the lock.
  // These transport APIs can also be called after ExitBootServices.
  if (EFI_ERROR (PpReadFlags (&Flags)) || EFI_ERROR (PpReadRequest (&Request))) {
    return TCG_PP_SUBMIT_REQUEST_TO_PREOS_GENERAL_FAILURE;
  }

  Request.PPRequest          = (UINT8)OperationRequest;
  Request.PPRequestParameter = RequestParameter;
  return EFI_ERROR (PpWriteRequest (&Request)) ? TCG_PP_SUBMIT_REQUEST_TO_PREOS_GENERAL_FAILURE : TCG_PP_SUBMIT_REQUEST_TO_PREOS_SUCCESS;
}

/** Pointer-form submission has identical validation and does not rewrite inputs.

  @param[in,out] OperationRequest  Operation code, left unchanged.
  @param[in,out] RequestParameter Operation parameter, left unchanged.
  @return                        PPI submission status.
**/
UINT32
Tcg2PhysicalPresenceLibSubmitRequestToPreOSFunctionEx (
  IN OUT UINT32  *OperationRequest,
  IN OUT UINT32  *RequestParameter
  )
{
  if ((OperationRequest == NULL) || (RequestParameter == NULL)) {
    return TCG_PP_SUBMIT_REQUEST_TO_PREOS_GENERAL_FAILURE;
  }

  return Tcg2PhysicalPresenceLibSubmitRequestToPreOSFunction (*OperationRequest, *RequestParameter);
}

/** Report conservative requirements when flags cannot be trusted.

  @return Stored flags with all console-confirmation requirements asserted.
**/
UINT32
EFIAPI
Tcg2PhysicalPresenceLibGetManagementFlags (
  VOID
  )
{
  EFI_TCG2_PHYSICAL_PRESENCE_FLAGS  Flags;

  if (EFI_ERROR (PpReadFlags (&Flags))) {
    return PP_REQUIRED_FLAGS;
  }

  // The actual authorization policy is stricter than legacy stored waivers.
  return Flags.PPFlags | PP_REQUIRED_FLAGS;
}

/** Advertise only implemented operations and the platform's strict PP policy.

  @param[in] OperationRequest  Full-width operation code.
  @return                     PPI confirmation requirement or NOT_IMPLEMENTED.
**/
UINT32
EFIAPI
Tcg2PhysicalPresenceLibGetUserConfirmationStatusFunction (
  IN UINT32  OperationRequest
  )
{
  if (!PpKnownOperation (OperationRequest)) {
    return TCG_PP_GET_USER_CONFIRMATION_NOT_IMPLEMENTED;
  }

  return (OperationRequest == TCG2_PHYSICAL_PRESENCE_NO_ACTION) ?
         TCG_PP_GET_USER_CONFIRMATION_ALLOWED_AND_PPUSER_NOT_REQUIRED :
         TCG_PP_GET_USER_CONFIRMATION_ALLOWED_AND_PPUSER_REQUIRED;
}
