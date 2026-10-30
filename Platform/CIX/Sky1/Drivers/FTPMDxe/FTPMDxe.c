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
#include <Library/DevicePathLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/tee_client_api.h>
#include <Protocol/FTPMInitProtocol.h>
#include <Protocol/LoadedImage.h>
#include "../../Library/OpteeClientApiLib/OpteeClientDiagnostics.h"
#include <Library/Tpm2CommandLib.h>
#include "../../Library/PlatformBootManagerLib/TpmBootCap.h"

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
STATIC EFI_EVENT     mBeforeExitBootServicesEvent;
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

/** Emit status-only diagnostics without allocation, including at EBS. **/
STATIC
VOID
ReportDecision (
  IN CONST CHAR8  *Decision,
  IN UINTN        Status,
  IN BOOLEAN      TeecStatus,
  IN BOOLEAN      SerialOnly
  )
{
  CHAR8   Text[192];
  CHAR16  ConsoleText[192];
  UINTN   Length;
  UINTN   Index;

  if (TeecStatus) {
    Length = AsciiSPrint (Text, sizeof (Text), "TPM FTPMDxe: %a (0x%08x)\r\n", Decision, (UINT32)Status);
  } else {
    Length = AsciiSPrint (Text, sizeof (Text), "TPM FTPMDxe: %a (%r)\r\n", Decision, Status);
  }

  SerialPortWrite ((UINT8 *)Text, Length);
  if (!SerialOnly && (gST != NULL) && (gST->ConOut != NULL)) {
    for (Index = 0; Index <= Length; Index++) {
      ConsoleText[Index] = (CHAR16)(UINT8)Text[Index];
    }

    gST->ConOut->OutputString (gST->ConOut, ConsoleText);
  }
}

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
    ReportDecision ("session open failed, TPM skipped", mOpenResult, TRUE, FALSE);
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

/** Private transport used only by this module's pre-publication cap. **/
EFI_STATUS
EFIAPI
FtpmResidentSubmitCommand (
  IN UINT32      InputSize,
  IN UINT8       *Input,
  IN OUT UINT32  *OutputSize,
  OUT UINT8      *Output
  )
{
  return FtpmSubmitCommand (&mFtpmProtocol, InputSize, Input, OutputSize, Output);
}

BOOLEAN
EFIAPI
FtpmResidentReady (
  VOID
  )
{
  return FtpmGetStatus (&mFtpmProtocol);
}

/**
  Runs before DXE Core accepts the OS memory-map key, so before Tcg2Dxe's
  TPL_NOTIFY final measurements submit through this transport. Console
  output allocates pool; keep client diagnostics on the serial port only.
**/
STATIC
VOID
EFIAPI
OnBeforeExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  OpteeClientApiExitBootServices ();
}

/**
  Runs at TPL_CALLBACK, after Tcg2Dxe's TPL_NOTIFY final measurements.
  Preserve failed-close ownership but allow OS handoff: the OS may boot
  without a TPM if it cannot open the single-session TA. No variable calls.
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
    // The client uses static SHM and allocation records reserved at init.
    Result = OpteeClientCloseSession (&mSession);
    if (Result != TEEC_SUCCESS) {
      ReportDecision ("EBS close failed, continuing OS handoff", Result, TRUE, TRUE);
      return;
    }

    mSessionOpen = FALSE;
  }

  // The client's ordinary finalizer frees pool memory. EBS has already
  // validated the memory map: retain inert bookkeeping instead. Linux's
  // OP-TEE probe drains stale SHM cache entries during ownership transfer.
  mClientReady = FALSE;
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

STATIC
EFI_STATUS
StartResidentDriver (
  IN EFI_HANDLE  ImageHandle
  )
{
  EFI_STATUS   Status;
  EFI_HANDLE   Handle;
  EFI_EVENT    Event;
  TEEC_Result  Result;
  EFI_STATUS   CapStatus;

  // Never open the single-session TA without installed handoff handlers.
  Status = gBS->CreateEventEx (
                               EVT_NOTIFY_SIGNAL,
                               TPL_CALLBACK,
                               OnBeforeExitBootServices,
                               NULL,
                               &gEfiEventBeforeExitBootServicesGuid,
                               &mBeforeExitBootServicesEvent
                               );
  if (EFI_ERROR (Status)) {
    ReportDecision ("EBS callback unavailable, TPM skipped", Status, FALSE, FALSE);
    return Status;
  }

  Status = gBS->CreateEventEx (
                               EVT_NOTIFY_SIGNAL,
                               TPL_CALLBACK,
                               OnExitBootServices,
                               NULL,
                               &gEfiEventExitBootServicesGuid,
                               &mExitBootServicesEvent
                               );
  if (EFI_ERROR (Status)) {
    gBS->CloseEvent (mBeforeExitBootServicesEvent);
    ReportDecision ("EBS callback unavailable, TPM skipped", Status, FALSE, FALSE);
    return Status;
  }

  mClientStatus = OpteeClientApiInitialize (ImageHandle);
  if (!EFI_ERROR (mClientStatus)) {
    Result = TEEC_InitializeContext (NULL, &mContext);
    if (Result == TEEC_SUCCESS) {
      mClientReady = TRUE;
      FtpmOpenSession (&mFtpmProtocol);
    } else {
      ReportDecision ("context initialization failed, TPM skipped", Result, TRUE, FALSE);
    }
  } else {
    ReportDecision ("client unavailable, TPM skipped", mClientStatus, FALSE, FALSE);
  }

  PublishStatus ();

  // Diagnostics may be refreshed at ReadyToBoot, never at EBS.
  Event  = NULL;
  Status = EfiCreateEventReadyToBootEx (TPL_CALLBACK, OnReadyToBoot, NULL, &Event);
  if (EFI_ERROR (Status)) {
    ReportDecision ("ReadyToBoot diagnostics skipped", Status, FALSE, FALSE);
  }

  // Publish unavailable status for consumers whose DXE depex uses the vendor
  // FTpmDeviceLib. BDS uses OptionalFtpmDeviceLib and must not wait for this
  // protocol: publication itself can fail after a live TPM was detected.
  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                                                   &Handle,
                                                   &gCixFtpmInitProtocolGuid,
                                                   &mFtpmProtocol,
                                                   NULL
                                                   );
  if (EFI_ERROR (Status)) {
    if (mSessionOpen && mSessionUsable) {
      ReportDecision ("protocol installation failed, capping PCRs 0-7", Status, FALSE, FALSE);
      CapStatus = PlatformTpmCapPcrs ();
      if (EFI_ERROR (CapStatus)) {
        ReportDecision ("PCR cap unproven, boot stopped", CapStatus, FALSE, FALSE);
        for (;;) {
          CpuDeadLoop ();
        }
      }

      ReportDecision ("PCRs 0-7 capped, continuing without TPM protocol", EFI_SUCCESS, FALSE, FALSE);
    }
    // The resident image owns the live session, client records and EBS code.
    // Do not finalize: an unconfirmed close/cache drain cannot release SHM.
    mSessionUsable  = FALSE;
    mClientReady    = FALSE;
    mHandoffStarted = TRUE;
    if (Event != NULL) {
      gBS->CloseEvent (Event);
    }

    ReportDecision ("protocol installation failed, TPM unavailable", Status, FALSE, FALSE);
  }

  return Status;
}

typedef struct {
  EFI_HANDLE  Parent;
  EFI_HANDLE  Child;
  EFI_STATUS  Status;
  BOOLEAN     Completed;
} FTPM_RESIDENT_START;

/**
  Start a separate resident copy before creating callbacks or owning SHM.

  DXE Core's CoreStartImage (Image.c) marks the image started before entry
  and unloads drivers only on an entry error (lines 1703-1704, 1790-1791).
  CoreUnloadImage rejects a started driver with NULL Unload (1935-1942).
  The outer image therefore owns no client resources. Its child always
  returns success and retains callbacks/SHM even if publication failed;
  only the synchronous private handshake returns that error to dispatch.
**/
EFI_STATUS
EFIAPI
FtpmDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_LOADED_IMAGE_PROTOCOL  *Image;
  EFI_LOADED_IMAGE_PROTOCOL  *Parent;
  EFI_LOADED_IMAGE_PROTOCOL  *Child;
  EFI_DEVICE_PATH_PROTOCOL   *Path;
  EFI_DEVICE_PATH_PROTOCOL   *ParentPath;
  FTPM_RESIDENT_START         Start;
  FTPM_RESIDENT_START         *Request;
  EFI_HANDLE                 ChildHandle;
  EFI_STATUS                 Status;
  VOID                       *SavedOptions;
  UINT32                     SavedOptionsSize;
  UINTN                      PathSize;

  Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Image);
  if (EFI_ERROR (Status)) {
    ReportDecision ("image identity unavailable, TPM skipped", Status, FALSE, FALSE);
    return Status;
  }

  Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageDevicePathProtocolGuid, (VOID **)&Path);
  if (EFI_ERROR (Status)) {
    ReportDecision ("image path unavailable, TPM skipped", Status, FALSE, FALSE);
    return Status;
  }

  // A resident request must be shared with the actual same-path parent.
  // An arbitrary LoadOptions flag alone must not select the resident branch.
  if ((Image->LoadOptionsSize == sizeof (Start)) && (Image->LoadOptions != NULL) &&
      (Image->ParentHandle != NULL) &&
      !EFI_ERROR (gBS->HandleProtocol (Image->ParentHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Parent)) &&
      (Parent->LoadOptions == Image->LoadOptions) &&
      (Parent->LoadOptionsSize == sizeof (Start)) &&
      !EFI_ERROR (gBS->HandleProtocol (Image->ParentHandle, &gEfiLoadedImageDevicePathProtocolGuid, (VOID **)&ParentPath)))
  {
    PathSize = GetDevicePathSize (Path);
    if ((PathSize == GetDevicePathSize (ParentPath)) &&
        (CompareMem (Path, ParentPath, PathSize) == 0))
    {
      Request = Image->LoadOptions;
      if ((Request->Parent == Image->ParentHandle) && (Request->Child == ImageHandle) && !Request->Completed) {
        Image->LoadOptions = NULL;
        Image->LoadOptionsSize = 0;
        Image->Unload = NULL;
        Request->Status = StartResidentDriver (ImageHandle);
        Request->Completed = TRUE;
        return EFI_SUCCESS;
      }
    }
  }

  Status = gBS->LoadImage (FALSE, ImageHandle, Path, NULL, 0, &ChildHandle);
  if (EFI_ERROR (Status)) {
    ReportDecision ("resident image load failed, TPM skipped", Status, FALSE, FALSE);
    return Status;
  }

  Status = gBS->HandleProtocol (ChildHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Child);
  if (EFI_ERROR (Status)) {
    gBS->UnloadImage (ChildHandle);
    ReportDecision ("resident image identity unavailable, TPM skipped", Status, FALSE, FALSE);
    return Status;
  }

  ZeroMem (&Start, sizeof (Start));
  Start.Parent = ImageHandle;
  Start.Child = ChildHandle;
  Start.Status = EFI_DEVICE_ERROR;
  SavedOptions = Image->LoadOptions;
  SavedOptionsSize = Image->LoadOptionsSize;
  Image->LoadOptions = &Start;
  Image->LoadOptionsSize = sizeof (Start);
  Child->LoadOptions = &Start;
  Child->LoadOptionsSize = sizeof (Start);
  Status = gBS->StartImage (ChildHandle, NULL, NULL);
  Image->LoadOptions = SavedOptions;
  Image->LoadOptionsSize = SavedOptionsSize;
  // Entry-point errors unload the child, but pre-entry failures may not.
  // Reacquire its protocol rather than touching a possibly freed pointer.
  if (EFI_ERROR (Status)) {
    if (!EFI_ERROR (gBS->HandleProtocol (ChildHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Child))) {
      Child->LoadOptions = NULL;
      Child->LoadOptionsSize = 0;
      gBS->UnloadImage (ChildHandle);
    }

    ReportDecision ("resident image start failed, TPM skipped", Status, FALSE, FALSE);
    return Status;
  }

  Child->LoadOptions = NULL;
  Child->LoadOptionsSize = 0;
  if (!Start.Completed) {
    ReportDecision ("resident initialization unavailable, TPM skipped", EFI_DEVICE_ERROR, FALSE, FALSE);
    return EFI_DEVICE_ERROR;
  }

  return Start.Status;
}
