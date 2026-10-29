/** @file
  Firmware TPM on Sky1: loads CIX's fTPM trusted application into OP-TEE and
  exposes it to Tcg2Dxe through FTPM_INIT_PROTOCOL.

  The TA is a single-instance, keep-alive instance without multi-session
  support, and keeps its NV in OP-TEE's secure NOR storage. Firmware closes
  its session after Tcg2Dxe has recorded the ExitBootServices events, so the
  OS can open the same instance and observe the PCRs firmware extended.

  The outcome is published in the volatile variable FtpmStatus under
  gCixFtpmInitProtocolGuid, readable from the OS for diagnostics.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Guid/EventGroup.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/OpteeClientApiLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/tee_client_api.h>
#include <Protocol/FTPMInitProtocol.h>

//
// fTPM TA command: [in] memref[0] TPM command, [in/out] memref[1] response.
//
#define FTPM_TA_SUBMIT_COMMAND  0
#define FTPM_TA_MAX_COMMAND     0x1000
#define FTPM_TA_MAX_RESPONSE    0x1000

#define FTPM_STATUS_VARIABLE  L"FtpmStatus"

extern EFI_GUID  gOpteeFtpmTaGuid;

STATIC TEEC_Context  mContext;
STATIC TEEC_Session  mSession;
STATIC BOOLEAN       mClientReady;
STATIC BOOLEAN       mSessionOpen;
STATIC EFI_EVENT     mExitBootServicesEvent;
STATIC BOOLEAN       mSessionUsable;
STATIC BOOLEAN       mHandoffStarted;
STATIC BOOLEAN       mCommandBusy;
STATIC UINT8         mCommandBuffer[FTPM_TA_MAX_COMMAND];
STATIC UINT8         mResponseBuffer[FTPM_TA_MAX_RESPONSE];

//
// Diagnostics, published by PublishStatus().
//
STATIC EFI_STATUS   mClientStatus = EFI_NOT_STARTED;
STATIC TEEC_Result  mOpenResult;
STATIC UINT32       mOpenOrigin;
STATIC UINT32       mCommands;
STATIC UINT32       mFailures;
STATIC TEEC_Result  mLastFailure;
STATIC UINT32       mLastFailureOrigin;

/**
  Erase owned authorization-bearing copies even under link-time optimization.

  @param[in,out] Buffer  Storage owned by this driver.
  @param[in]     Size    Number of bytes to erase.
**/
STATIC
VOID
ScrubBuffer (
  IN OUT VOID  *Buffer,
  IN UINTN     Size
  )
{
  volatile UINT8  *Byte;

  Byte = (volatile UINT8 *)Buffer;
  while (Size-- != 0) {
    *Byte++ = 0;
  }
}

STATIC
VOID
PublishStatus (
  VOID
  )
{
  CHAR8  Text[256];
  UINTN  Length;

  Length = AsciiSPrint (
                        Text,
                        sizeof (Text),
                        "client=%r open=0x%x/%u session=%a commands=%u failures=%u last-failure=0x%x/%u\n",
                        mClientStatus,
                        mOpenResult,
                        mOpenOrigin,
                        mSessionOpen ? "open" : "closed",
                        mCommands,
                        mFailures,
                        mLastFailure,
                        mLastFailureOrigin
                        );
  gRT->SetVariable (
                    FTPM_STATUS_VARIABLE,
                    &gCixFtpmInitProtocolGuid,
                    EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
                    Length,
                    Text
                    );
}

STATIC
EFI_STATUS
EFIAPI
FtpmOpenSession (
  IN FTPM_INIT_PROTOCOL  *This
  )
{
  if (!mClientReady || mHandoffStarted) {
    return EFI_NOT_READY;
  }

  if (mSessionOpen) {
    return mSessionUsable ? EFI_SUCCESS : EFI_NOT_READY;
  }

  mOpenResult = TEEC_OpenSession (
                                  &mContext,
                                  &mSession,
                                  (CONST TEEC_UUID *)&gOpteeFtpmTaGuid,
                                  TEEC_LOGIN_PUBLIC,
                                  NULL,
                                  NULL,
                                  &mOpenOrigin
                                  );
  if (mOpenResult != TEEC_SUCCESS) {
    DEBUG ((DEBUG_ERROR, "fTPM: opening TA session failed: 0x%x (origin %u)\n", mOpenResult, mOpenOrigin));
    return EFI_DEVICE_ERROR;
  }

  mSessionOpen   = TRUE;
  mSessionUsable = TRUE;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
FtpmGetSession (
  IN  FTPM_INIT_PROTOCOL  *This,
  OUT TEEC_Context        *Context,
  OUT TEEC_Session        *Session
  )
{
  if ((Context == NULL) || (Session == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (!mSessionOpen || !mSessionUsable || mHandoffStarted) {
    return EFI_NOT_READY;
  }

  CopyMem (Context, &mContext, sizeof (mContext));
  CopyMem (Session, &mSession, sizeof (mSession));
  return EFI_SUCCESS;
}

STATIC
BOOLEAN
EFIAPI
FtpmGetStatus (
  IN FTPM_INIT_PROTOCOL  *This
  )
{
  return mClientReady && mSessionOpen && mSessionUsable && !mHandoffStarted;
}

/**
  Validate the complete TA frame before publishing any response bytes.

  @param[in]     This        Protocol instance.
  @param[in]     InputSize   Command bytes, including the ten-byte header.
  @param[in]     Input       TPM command in wire byte order.
  @param[in,out] OutputSize  Capacity on entry; validated or required size on exit.
  @param[out]    Output      Receives bytes only after a validated success.

  @retval EFI_SUCCESS            A complete, bounded TPM response was returned.
  @retval EFI_BUFFER_TOO_SMALL   A genuine short-buffer reply supplied its size.
  @retval EFI_INVALID_PARAMETER  The command or pointer contract is invalid.
  @retval EFI_NOT_READY          The firmware session is not usable.
  @retval EFI_DEVICE_ERROR       The TA failed or returned malformed framing.
**/
STATIC
EFI_STATUS
EFIAPI
FtpmSubmitCommand (
  IN     FTPM_INIT_PROTOCOL  *This,
  IN     UINT32              InputSize,
  IN     UINT8               *Input,
  IN OUT UINT32              *OutputSize,
  OUT    UINT8               *Output
  )
{
  TEEC_Operation  Operation;
  TEEC_Result     Result;
  UINT32          Origin;
  UINT32          Capacity;
  UINTN           ResponseSize;
  UINT16          Tag;
  EFI_STATUS      Status;

  if ((Input == NULL) || (OutputSize == NULL) ||
      ((Output == NULL) && (*OutputSize != 0)) ||
      (InputSize < sizeof (TPM2_COMMAND_HEADER)) ||
      (InputSize > sizeof (mCommandBuffer)))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (!FtpmGetStatus (This) || mCommandBusy) {
    return EFI_NOT_READY;
  }

  //
  // Snapshot before parsing. Both buffers are fixed DXE storage, including
  // during Tcg2Dxe's final EBS measurements; caller output is never input.
  //
  mCommandBusy = TRUE;
  CopyMem (mCommandBuffer, Input, InputSize);
  if (SwapBytes32 (ReadUnaligned32 ((CONST UINT32 *)(mCommandBuffer + 2))) != InputSize) {
    Status = EFI_INVALID_PARAMETER;
    goto Done;
  }

  Capacity    = MIN (*OutputSize, sizeof (mResponseBuffer));
  *OutputSize = 0;
  ZeroMem (mResponseBuffer, sizeof (mResponseBuffer));
  ZeroMem (&Operation, sizeof (Operation));
  Operation.paramTypes = TEEC_PARAM_TYPES (
                                           TEEC_MEMREF_TEMP_INPUT,
                                           TEEC_MEMREF_TEMP_INOUT,
                                           TEEC_NONE,
                                           TEEC_NONE
                                           );
  Operation.params[0].tmpref.buffer = mCommandBuffer;
  Operation.params[0].tmpref.size   = InputSize;
  Operation.params[1].tmpref.buffer = mResponseBuffer;
  Operation.params[1].tmpref.size   = Capacity;

  mCommands++;
  Origin       = TEEC_ORIGIN_API;
  Result       = TEEC_InvokeCommand (&mSession, FTPM_TA_SUBMIT_COMMAND, &Operation, &Origin);
  ResponseSize = Operation.params[1].tmpref.size;
  Status       = EFI_DEVICE_ERROR;
  if (Result == TEEC_ERROR_SHORT_BUFFER) {
    if ((ResponseSize > Capacity) && (ResponseSize >= sizeof (TPM2_RESPONSE_HEADER)) &&
        (ResponseSize <= sizeof (mResponseBuffer)))
    {
      *OutputSize = (UINT32)ResponseSize;
      Status      = EFI_BUFFER_TOO_SMALL;
      goto Done;
    }
  } else if (Result == TEEC_SUCCESS) {
    if ((ResponseSize >= sizeof (TPM2_RESPONSE_HEADER)) && (ResponseSize <= Capacity)) {
      Tag = SwapBytes16 (ReadUnaligned16 ((CONST UINT16 *)mResponseBuffer));
      if (((Tag == TPM_ST_NO_SESSIONS) || (Tag == TPM_ST_SESSIONS)) &&
          (SwapBytes32 (ReadUnaligned32 ((CONST UINT32 *)(mResponseBuffer + 2))) == ResponseSize))
      {
        CopyMem (Output, mResponseBuffer, ResponseSize);
        *OutputSize = (UINT32)ResponseSize;
        Status      = EFI_SUCCESS;
        goto Done;
      }
    }
  } else if ((Result == TEEC_ERROR_TARGET_DEAD) || (Result == TEEC_ERROR_COMMUNICATION) ||
             (Result == TEEC_ERROR_BAD_STATE) || (Result == TEEC_ERROR_SECURITY))
  {
    mSessionUsable = FALSE;
  }

  if ((Result == TEEC_SUCCESS) || (Result == TEEC_ERROR_SHORT_BUFFER)) {
    Result = TEEC_ERROR_BAD_FORMAT;
  }

  // Diagnostics only: variable services may themselves use OP-TEE at EBS.
  if (mFailures++ == 0) {
    mLastFailure       = Result;
    mLastFailureOrigin = Origin;
  }

Done:
  ScrubBuffer (mCommandBuffer, sizeof (mCommandBuffer));
  ScrubBuffer (mResponseBuffer, sizeof (mResponseBuffer));
  mCommandBusy = FALSE;
  return Status;
}

STATIC FTPM_INIT_PROTOCOL  mFtpmProtocol = {
  FtpmGetSession,
  FtpmOpenSession,
  FtpmGetStatus,
  FtpmSubmitCommand
};

/**
  Runs at TPL_CALLBACK, after Tcg2Dxe's TPL_NOTIFY final measurements.
  A callback cannot veto EBS: halt on unconfirmed close rather than hand
  Linux a still-owned single-instance TA. No variable or allocation calls.
**/
STATIC
VOID
EFIAPI
OnExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  TEEC_Result  Result;

  mHandoffStarted = TRUE;
  mSessionUsable  = FALSE;
  if (mSessionOpen) {
    // C1's client implementation must reserve its close message during init.
    // No driver-owned buffer can be passed through this fixed API.
    Result = OpteeClientCloseSession (&mSession);
    if (Result != TEEC_SUCCESS) {
      DEBUG ((DEBUG_ERROR, "fTPM: EBS close failed: 0x%x; refusing OS handoff\n", Result));
      CpuDeadLoop ();
      return;
    }

    mSessionOpen = FALSE;
  }

  if (mClientReady) {
    TEEC_FinalizeContext (&mContext);
    if (!OpteeClientApiFinalize ()) {
      DEBUG ((DEBUG_ERROR, "fTPM: EBS client finalization failed; refusing OS handoff\n"));
      CpuDeadLoop ();
      return;
    }

    mClientReady = FALSE;
  }
}

STATIC
VOID
EFIAPI
OnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  PublishStatus ();
}

EFI_STATUS
EFIAPI
FtpmDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle;
  EFI_EVENT   Event;

  // Never open the single-session TA without an installed handoff handler.
  Status = gBS->CreateEventEx (
                               EVT_NOTIFY_SIGNAL,
                               TPL_CALLBACK,
                               OnExitBootServices,
                               NULL,
                               &gEfiEventExitBootServicesGuid,
                               &mExitBootServicesEvent
                               );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mClientStatus = OpteeClientApiInitialize (ImageHandle);
  if (!EFI_ERROR (mClientStatus) && (TEEC_InitializeContext (NULL, &mContext) == TEEC_SUCCESS)) {
    mClientReady = TRUE;
    FtpmOpenSession (&mFtpmProtocol);
  } else {
    DEBUG ((DEBUG_ERROR, "fTPM: OP-TEE client unavailable: %r\n", mClientStatus));
  }

  PublishStatus ();

  // Diagnostics may be refreshed at ReadyToBoot, never at EBS.
  Event  = NULL;
  Status = EfiCreateEventReadyToBootEx (TPL_CALLBACK, OnReadyToBoot, NULL, &Event);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "fTPM: cannot refresh ReadyToBoot diagnostics: %r\n", Status));
  }

  //
  // Install unconditionally: every user of FTpmDeviceLib depends on this
  // protocol, including boot-critical drivers. A missing fTPM is reported
  // through GetFTPMStatus instead.
  //
  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                                                   &Handle,
                                                   &gCixFtpmInitProtocolGuid,
                                                   &mFtpmProtocol,
                                                   NULL
                                                   );
  if (EFI_ERROR (Status)) {
    OnExitBootServices (NULL, NULL);
    gBS->CloseEvent (mExitBootServicesEvent);
    if (Event != NULL) {
      gBS->CloseEvent (Event);
    }
  }

  return Status;
}
