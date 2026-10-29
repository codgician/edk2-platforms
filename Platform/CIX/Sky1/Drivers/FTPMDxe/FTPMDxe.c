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

#define FTPM_STATUS_VARIABLE  L"FtpmStatus"

extern EFI_GUID  gOpteeFtpmTaGuid;

STATIC TEEC_Context  mContext;
STATIC TEEC_Session  mSession;
STATIC BOOLEAN       mClientReady;
STATIC BOOLEAN       mSessionOpen;
STATIC EFI_EVENT     mExitBootServicesEvent;

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
  if (mSessionOpen) {
    return EFI_SUCCESS;
  }

  if (!mClientReady) {
    return EFI_NOT_READY;
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

  mSessionOpen = TRUE;
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
  if (!mSessionOpen) {
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
  return mSessionOpen;
}

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
  uint32_t        Origin;

  if (!mSessionOpen) {
    return EFI_NOT_READY;
  }

  ZeroMem (&Operation, sizeof (Operation));
  Operation.paramTypes = TEEC_PARAM_TYPES (
                           TEEC_MEMREF_TEMP_INPUT,
                           TEEC_MEMREF_TEMP_INOUT,
                           TEEC_NONE,
                           TEEC_NONE
                           );
  Operation.params[0].tmpref.buffer = Input;
  Operation.params[0].tmpref.size   = InputSize;
  Operation.params[1].tmpref.buffer = Output;
  Operation.params[1].tmpref.size   = *OutputSize;

  mCommands++;
  Result = TEEC_InvokeCommand (&mSession, FTPM_TA_SUBMIT_COMMAND, &Operation, &Origin);
  if (Result == TEEC_ERROR_SHORT_BUFFER) {
    *OutputSize = (UINT32)Operation.params[1].tmpref.size;
    return EFI_BUFFER_TOO_SMALL;
  }

  if (Result != TEEC_SUCCESS) {
    DEBUG ((DEBUG_ERROR, "fTPM: command failed: 0x%x (origin %u)\n", Result, Origin));
    //
    // Only record: Tcg2Dxe also submits commands during ExitBootServices,
    // where variable services (served from OP-TEE) must not be called.
    //
    if (mFailures++ == 0) {
      mLastFailure       = Result;
      mLastFailureOrigin = Origin;
    }

    return EFI_DEVICE_ERROR;
  }

  *OutputSize = (UINT32)Operation.params[1].tmpref.size;
  return EFI_SUCCESS;
}

STATIC FTPM_INIT_PROTOCOL  mFtpmProtocol = {
  FtpmGetSession,
  FtpmOpenSession,
  FtpmGetStatus,
  FtpmSubmitCommand
};

/**
  Runs at TPL_CALLBACK, after Tcg2Dxe's TPL_NOTIFY handlers have measured
  the ExitBootServices events. Must not call variable services: they are
  served from OP-TEE and boot services are being torn down.
**/
STATIC
VOID
EFIAPI
OnExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  if (mSessionOpen) {
    TEEC_CloseSession (&mSession);
    mSessionOpen = FALSE;
  }

  if (mClientReady) {
    TEEC_FinalizeContext (&mContext);
    OpteeClientApiFinalize ();
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

  mClientStatus = OpteeClientApiInitialize (ImageHandle);
  if (!EFI_ERROR (mClientStatus) && (TEEC_InitializeContext (NULL, &mContext) == TEEC_SUCCESS)) {
    mClientReady = TRUE;
    FtpmOpenSession (&mFtpmProtocol);
  } else {
    DEBUG ((DEBUG_ERROR, "fTPM: OP-TEE client unavailable: %r\n", mClientStatus));
  }

  PublishStatus ();

  //
  // Refresh the status once DXE-phase measurements are done; ReadyToBoot
  // still allows variable services.
  //
  Status = EfiCreateEventReadyToBootEx (TPL_CALLBACK, OnReadyToBoot, NULL, &Event);
  ASSERT_EFI_ERROR (Status);

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  OnExitBootServices,
                  NULL,
                  &gEfiEventExitBootServicesGuid,
                  &mExitBootServicesEvent
                  );
  ASSERT_EFI_ERROR (Status);

  //
  // Install unconditionally: every user of FTpmDeviceLib depends on this
  // protocol, including boot-critical drivers. A missing fTPM is reported
  // through GetFTPMStatus instead.
  //
  Handle = NULL;
  return gBS->InstallMultipleProtocolInterfaces (
                &Handle,
                &gCixFtpmInitProtocolGuid,
                &mFtpmProtocol,
                NULL
                );
}
