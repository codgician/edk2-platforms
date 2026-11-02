/** @file
  Firmware TPM on Sky1: loads CIX's fTPM trusted application into OP-TEE and
  exposes it to Tcg2Dxe through FTPM_INIT_PROTOCOL.

  The TA is a single-instance, keep-alive instance without multi-session
  support, and keeps its NV in OP-TEE's secure NOR storage. Firmware closes
  its session after Tcg2Dxe has recorded the ExitBootServices events, so the
  OS can open the same instance and observe the PCRs firmware extended.

  The outcome is published in the volatile variable FtpmStatus under
  gCixFtpmInitProtocolGuid, readable from the OS for diagnostics. From
  ReadyToBoot on, the OEM ACPI table OEMF also carries it as one ASCII line,
  rewritten in place through ExitBootServices, where variables are unsafe.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>
#include <Guid/EventGroup.h>
#include <IndustryStandard/Acpi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/OpteeClientApiLib.h>
#include <Library/OpteeTrustedAppGuids.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/tee_client_api.h>
#include <Protocol/FTPMInitProtocol.h>
#include <Protocol/AcpiSystemDescriptionTable.h>
#include <Protocol/AcpiTable.h>
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

//
// "OEMx" signatures are reserved for OEM tables; Linux exposes this one at
// /sys/firmware/acpi/tables/OEMF and never loads it as AML.
//
#define FTPM_ACPI_STATUS_SIGNATURE  SIGNATURE_32 ('O', 'E', 'M', 'F')
#define FTPM_ACPI_STATUS_TEXT_SIZE  384

#pragma pack(1)
typedef struct {
  EFI_ACPI_DESCRIPTION_HEADER    Header;
  CHAR8                          Text[FTPM_ACPI_STATUS_TEXT_SIZE];
} FTPM_ACPI_STATUS_TABLE;
#pragma pack()

STATIC TEEC_Context  mContext;
STATIC TEEC_Session  mSession;
STATIC BOOLEAN       mSessionOpen;
STATIC EFI_EVENT     mBeforeExitBootServicesEvent;
STATIC EFI_EVENT     mExitBootServicesEvent;
STATIC BOOLEAN       mSessionUsable;
STATIC BOOLEAN       mCommandBusy;
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
STATIC TEEC_Result  mCloseResult;
STATIC UINT32       mCloses;

//
// The installed OEMF copy, written in place after ReadyToBoot.
//
STATIC FTPM_ACPI_STATUS_TABLE  *mAcpiStatus;
STATIC BOOLEAN                 mAcpiStatusAttempted;
STATIC BOOLEAN                 mAcpiStatusBusy;
STATIC BOOLEAN                 mAcpiStatusDirty;
STATIC CONST CHAR8             *mStage = "rtb";

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

/**
  Render the status line into Table and fix its checksum. Plain memory
  writes only (no allocation, protocol, variable or console), so it is safe
  inside ExitBootServices notifications.
**/
STATIC
VOID
RenderAcpiStatus (
  OUT FTPM_ACPI_STATUS_TABLE  *Table
  )
{
  OPTEE_CLIENT_DIAGNOSTICS  Client;
  UINTN                     Length;

  OpteeClientGetDiagnostics (&Client);
  ZeroMem (Table->Text, sizeof (Table->Text));
  Length = AsciiSPrint (
                        Table->Text,
                        sizeof (Table->Text),
                        "FTPM stage=%a client=%r open=0x%x/%u commands=%u failures=%u first-failure=0x%x/%u "
                        "closes=%u close=0x%x client-broken=%u broken-call=%u broken-rpcs=%u "
                        "rejects=%u first-reject=\"%a\" value=0x%lx status=0x%lx",
                        mStage,
                        mClientStatus,
                        mOpenResult,
                        mOpenOrigin,
                        mCommands,
                        mFailures,
                        mLastFailure,
                        mLastFailureOrigin,
                        mCloses,
                        mCloseResult,
                        (UINT32)Client.Broken,
                        Client.BrokenCall,
                        Client.BrokenRpcs,
                        Client.Rejects,
                        (Client.FirstCheck != NULL) ? Client.FirstCheck : "-",
                        Client.FirstValue,
                        Client.FirstStatus
                        );

  // PrintLib would expand "\n" to CR LF; end the line Unix-style. The
  // buffer was zeroed, so the terminator and padding stay NUL.
  if (Length < sizeof (Table->Text) - 1) {
    Table->Text[Length] = '\n';
  }

  Table->Header.Checksum = 0;
  Table->Header.Checksum = CalculateCheckSum8 ((CONST UINT8 *)Table, sizeof (*Table));
}

/**
  Refresh the installed table, optionally advancing the stage. A failure
  submitted from a higher-TPL notification while a refresh is in progress
  marks it dirty and the outer refresh renders again.

  @param[in] Stage  New stage literal, or NULL to keep the current one.
**/
STATIC
VOID
UpdateAcpiStatus (
  IN CONST CHAR8  *Stage OPTIONAL
  )
{
  if (Stage != NULL) {
    mStage = Stage;
  }

  if (mAcpiStatus == NULL) {
    return;
  }

  mAcpiStatusDirty = TRUE;
  if (mAcpiStatusBusy) {
    return;
  }

  mAcpiStatusBusy = TRUE;
  while (mAcpiStatusDirty) {
    mAcpiStatusDirty = FALSE;
    RenderAcpiStatus (mAcpiStatus);
  }

  mAcpiStatusBusy = FALSE;
}

/**
  Install OEMF and keep the address of AcpiTableDxe's copy for in-place
  updates. Any failure only loses the table; boot continues.
**/
STATIC
VOID
InstallAcpiStatus (
  VOID
  )
{
  EFI_ACPI_TABLE_PROTOCOL  *AcpiTable;
  EFI_ACPI_SDT_PROTOCOL    *AcpiSdt;
  EFI_ACPI_SDT_HEADER      *Installed;
  EFI_ACPI_TABLE_VERSION   Version;
  FTPM_ACPI_STATUS_TABLE   Table;
  UINTN                    TableKey;
  UINTN                    Key;
  UINTN                    Index;
  EFI_STATUS               Status;

  Status = gBS->LocateProtocol (&gEfiAcpiTableProtocolGuid, NULL, (VOID **)&AcpiTable);
  if (!EFI_ERROR (Status)) {
    Status = gBS->LocateProtocol (&gEfiAcpiSdtProtocolGuid, NULL, (VOID **)&AcpiSdt);
  }

  if (EFI_ERROR (Status)) {
    ReportDecision ("ACPI status table unavailable", Status, FALSE, TRUE);
    return;
  }

  ZeroMem (&Table, sizeof (Table));
  Table.Header.Signature       = FTPM_ACPI_STATUS_SIGNATURE;
  Table.Header.Length          = sizeof (Table);
  Table.Header.Revision        = 1;
  CopyMem (Table.Header.OemId, "CIXTEK", sizeof (Table.Header.OemId));
  Table.Header.OemTableId      = SIGNATURE_64 ('F', 'T', 'P', 'M', 'D', 'I', 'A', 'G');
  Table.Header.OemRevision     = 1;
  Table.Header.CreatorId       = SIGNATURE_32 ('C', 'I', 'X', ' ');
  Table.Header.CreatorRevision = 1;
  RenderAcpiStatus (&Table);
  Status = AcpiTable->InstallAcpiTable (AcpiTable, &Table, sizeof (Table), &TableKey);
  if (EFI_ERROR (Status)) {
    ReportDecision ("ACPI status table not installed", Status, FALSE, TRUE);
    return;
  }

  for (Index = 0; !EFI_ERROR (AcpiSdt->GetAcpiTable (Index, &Installed, &Version, &Key)); Index++) {
    if (Key == TableKey) {
      if ((Installed->Signature == FTPM_ACPI_STATUS_SIGNATURE) && (Installed->Length == sizeof (Table))) {
        mAcpiStatus = (FTPM_ACPI_STATUS_TABLE *)Installed;
      }

      break;
    }
  }

  if (mAcpiStatus == NULL) {
    // A frozen ReadyToBoot snapshot would misreport the EBS windows.
    AcpiTable->UninstallAcpiTable (AcpiTable, TableKey);
    ReportDecision ("ACPI status table copy not found, removed", EFI_NOT_FOUND, FALSE, TRUE);
  }
}

/** The single TA session is private to this driver; never hand it out. **/
STATIC
EFI_STATUS
EFIAPI
FtpmOpenSession (
  IN FTPM_INIT_PROTOCOL  *This
  )
{
  return EFI_UNSUPPORTED;
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
  return EFI_UNSUPPORTED;
}

STATIC
BOOLEAN
EFIAPI
FtpmGetStatus (
  IN FTPM_INIT_PROTOCOL  *This
  )
{
  return mSessionUsable;
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
      (InputSize > FTPM_TA_MAX_COMMAND) ||
      (SwapBytes32 (ReadUnaligned32 ((CONST UINT32 *)(Input + 2))) != InputSize))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (!FtpmGetStatus (This) || mCommandBusy) {
    return EFI_NOT_READY;
  }

  //
  // The client copies the command into shared memory before the TA runs.
  // The response buffer is fixed DXE storage, including during Tcg2Dxe's
  // final EBS measurements; caller output is never written before checks.
  //
  mCommandBusy = TRUE;

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
  Operation.params[0].tmpref.buffer = Input;
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

  UpdateAcpiStatus (NULL);

Done:
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
  UpdateAcpiStatus ("before-ebs");
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

  // Records the final measurements even if the close below never returns.
  UpdateAcpiStatus ("ebs");
  mSessionUsable = FALSE;
  if (mSessionOpen) {
    // The client uses static SHM and allocation records reserved at init.
    Result       = OpteeClientCloseSession (&mSession);
    mCloseResult = Result;
    mCloses++;
    if (Result != TEEC_SUCCESS) {
      UpdateAcpiStatus ("ebs-close");
      ReportDecision ("EBS close failed, continuing OS handoff", Result, TRUE, TRUE);
      return;
    }

    mSessionOpen = FALSE;
  }

  UpdateAcpiStatus ("ebs-close");
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
  if (!mAcpiStatusAttempted) {
    mAcpiStatusAttempted = TRUE;
    InstallAcpiStatus ();
  }

  UpdateAcpiStatus ("rtb");
}

/**
  Fails only before any callback or OP-TEE resource exists. Afterwards it
  returns success even if publication fails: DXE Core unloads a driver only
  on an entry-point error, and the EBS callbacks and session must remain.
**/
EFI_STATUS
EFIAPI
FtpmDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS   Status;
  EFI_HANDLE   Handle;
  EFI_EVENT    Event;
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
    // Cannot fail once the client is initialized.
    TEEC_InitializeContext (NULL, &mContext);
    mOpenResult = TEEC_OpenSession (
                                    &mContext,
                                    &mSession,
                                    (CONST TEEC_UUID *)&gOpteeFtpmTaGuid,
                                    TEEC_LOGIN_PUBLIC,
                                    NULL,
                                    NULL,
                                    &mOpenOrigin
                                    );
    mSessionOpen   = (mOpenResult == TEEC_SUCCESS);
    mSessionUsable = mSessionOpen;
    if (!mSessionOpen) {
      ReportDecision ("session open failed, TPM skipped", mOpenResult, TRUE, FALSE);
    }
  } else {
    ReportDecision ("client unavailable, TPM skipped", mClientStatus, FALSE, FALSE);
  }

  PublishStatus ();

  // Diagnostics may be refreshed at ReadyToBoot, never at EBS.
  Status = EfiCreateEventReadyToBootEx (TPL_CALLBACK, OnReadyToBoot, NULL, &Event);
  if (EFI_ERROR (Status)) {
    ReportDecision ("ReadyToBoot diagnostics skipped", Status, FALSE, FALSE);
  }

  // Publish unavailable status for consumers whose DXE depex uses
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
    if (mSessionUsable) {
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
    // Keep the live session, client records and EBS code resident. Do not
    // finalize: an unconfirmed close/cache drain cannot release SHM.
    mSessionUsable = FALSE;
    ReportDecision ("protocol installation failed, TPM unavailable", Status, FALSE, FALSE);
  }

  return EFI_SUCCESS;
}
