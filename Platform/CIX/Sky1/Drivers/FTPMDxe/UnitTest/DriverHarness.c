/** @file
  Compile production code unchanged; mock only its external services.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include "DriverHarness.h"
#include <setjmp.h>
#include <Library/Tpm2CommandLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiLib.h>
#include <Library/SerialPortLib.h>

#include "../FTPMDxe.c"

EFI_GUID                     gOpteeFtpmTaGuid;
EFI_GUID                     gCixFtpmInitProtocolGuid = CIX_FTPM_INIT_PROTOCOL_GUID;
EFI_GUID                     gEfiEventBeforeExitBootServicesGuid;
EFI_GUID                     gEfiEventExitBootServicesGuid;
STATIC EFI_SYSTEM_TABLE      mSystemTable;
EFI_SYSTEM_TABLE             *gST = &mSystemTable;
STATIC EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL mConOut;
STATIC EFI_EVENT_NOTIFY      mReadyCallback;
CHAR8                        TestSerial[4096];
UINTN                        TestConsoleWrites;
EFI_STATUS                   TestClientResult;
EFI_STATUS                   TestReadyEventResult;
UINTN                        TestClosedEvents;
UINTN                        TestCapExtends;
EFI_STATUS                   TestCapResult;
TEEC_Result                  TestCapDiscoveryResult;
BOOLEAN                      TestCapMalformedFrame;
UINTN                        TestCapFailPcr;
STATIC jmp_buf               mCapTerminal;
STATIC BOOLEAN               mExpectCapTerminal;
STATIC EFI_BOOT_SERVICES     mBootServices;
STATIC EFI_RUNTIME_SERVICES  mRuntimeServices;
EFI_BOOT_SERVICES            *gBS = &mBootServices;
EFI_RUNTIME_SERVICES         *gRT = &mRuntimeServices;

FTPM_INIT_PROTOCOL       *TestProtocol;
UINT8                    TestResponse[TEST_TPM_MAX];
UINTN                    TestResponseBytes;
UINTN                    TestReturnedSize;
TEEC_Result              TestInvokeResult;
TEEC_Result              TestCloseResult;
TEEC_Result              TestOpenResult;
BOOLEAN                  TestCloseSawReadinessRevoked;
UINT32                   TestLastDiagnosticCommands;
UINT32                   TestLastDiagnosticFailures;
EFI_STATUS               TestEventResult;
UINTN                    TestEventFailCall;
UINTN                    TestEventCalls;
BOOLEAN                  TestSerialOnly;
UINTN                    TestInvocations;
EFI_STATUS               TestInstallResult;
UINTN                    TestCheckedCloses;
UINTN                    TestVariableWrites;
BOOLEAN                  TestAtEbs;
BOOLEAN                  TestHalted;
BOOLEAN                  TestCheckZeroResponse;
UINT8                    *TestCallerOutput;
UINT8                    *TestSavedCommand;
UINT8                    *TestSavedResponse;
UINTN                    TestSavedCommandSize;
UINTN                    TestSavedResponseSize;
STATIC EFI_EVENT_NOTIFY  mEbsCallback;
STATIC EFI_EVENT_NOTIFY  mBeforeEbsCallback;
STATIC BOOLEAN           mBeforeEbsSignaled;

VOID
TestRequire (
  BOOLEAN  Condition
  )
{
  if (!Condition) {
    abort ();
  }
}

BOOLEAN
TestAllZero (
  CONST UINT8  *Buffer,
  UINTN        Size
  )
{
  for (UINTN Index = 0; Index < Size; Index++) {
    if (Buffer[Index] != 0) {
      return FALSE;
    }
  }

  return TRUE;
}

VOID
TestHeader (
  UINT8   *Buffer,
  UINT16  Tag,
  UINT32  Size
  )
{
  Buffer[0] = (UINT8)(Tag >> 8);
  Buffer[1] = (UINT8)Tag;
  Buffer[2] = (UINT8)(Size >> 24);
  Buffer[3] = (UINT8)(Size >> 16);
  Buffer[4] = (UINT8)(Size >> 8);
  Buffer[5] = (UINT8)Size;
  memset (Buffer + 6, 0, 4);
}

STATIC EFI_STATUS EFIAPI
MockSetVariable (
  CHAR16    *Name,
  EFI_GUID  *Guid,
  UINT32    Attributes,
  UINTN     Size,
  VOID      *Data
  )
{
  TestRequire (!TestAtEbs);
  TestRequire (Attributes == (EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS));
  TestRequire (Size != 0 && Size < 256);
  TestRequire (memcmp (Name, FTPM_STATUS_VARIABLE, sizeof (FTPM_STATUS_VARIABLE)) == 0);
  TestRequire (Guid == &gCixFtpmInitProtocolGuid);
  TestLastDiagnosticCommands = mCommands;
  TestLastDiagnosticFailures = mFailures;
  TestVariableWrites++;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
MockCreateEventEx (
  UINT32            Type,
  EFI_TPL           Tpl,
  EFI_EVENT_NOTIFY  Notify,
  CONST VOID        *Context,
  CONST EFI_GUID    *Group,
  EFI_EVENT         *Event
  )
{
  EFI_STATUS  Status;

  TestRequire (!TestAtEbs && Tpl == TPL_CALLBACK && Type == EVT_NOTIFY_SIGNAL);
  TestRequire (Group == &gEfiEventBeforeExitBootServicesGuid || Group == &gEfiEventExitBootServicesGuid);
  TestEventCalls++;
  Status = ((TestEventFailCall == 0) || (TestEventFailCall == TestEventCalls)) ? TestEventResult : EFI_SUCCESS;
  if (Group == &gEfiEventBeforeExitBootServicesGuid) {
    if (!EFI_ERROR (Status)) {
      mBeforeEbsCallback = Notify;
      *Event             = (EFI_EVENT)(UINTN)3;
    }

    return Status;
  }

  if (!EFI_ERROR (Status)) {
    mEbsCallback = Notify;
    *Event       = (EFI_EVENT)(UINTN)1;
  }
  return Status;
}

STATIC EFI_STATUS EFIAPI
MockCloseEvent (
  EFI_EVENT  Event
  )
{
  TestRequire (!TestAtEbs && (UINTN)Event == 3);
  TestClosedEvents++;
  mBeforeEbsCallback = NULL;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
MockInstall (
  EFI_HANDLE  *Handle,
  ...
  )
{
  va_list  Args;

  TestRequire (!TestAtEbs);
  va_start (Args, Handle);
  TestRequire (va_arg (Args, EFI_GUID *) == &gCixFtpmInitProtocolGuid);
  TestProtocol = va_arg (Args, FTPM_INIT_PROTOCOL *);
  TestRequire (va_arg (Args, VOID *) == NULL);
  va_end (Args);
  if (EFI_ERROR (TestInstallResult)) {
    TestProtocol = NULL;
  }

  return TestInstallResult;
}

EFI_STATUS EFIAPI
EfiCreateEventReadyToBootEx (
  EFI_TPL           Tpl,
  EFI_EVENT_NOTIFY  Notify,
  VOID              *Context,
  EFI_EVENT         *Event
  )
{
  TestRequire (!TestAtEbs && Tpl == TPL_CALLBACK);
  if (!EFI_ERROR (TestReadyEventResult)) {
    mReadyCallback = Notify;
    *Event = (EFI_EVENT)(UINTN)2;
  }
  return TestReadyEventResult;
}

EFI_STATUS
OpteeClientApiInitialize (
  EFI_HANDLE  ImageHandle
  )
{
  TestRequire (!TestAtEbs);
  return TestClientResult;
}

TEEC_Result
TEEC_InitializeContext (
  const char    *Name,
  TEEC_Context  *Context
  )
{
  TestRequire (!TestAtEbs);
  Context->fd = 1;
  return TEEC_SUCCESS;
}

TEEC_Result
TEEC_OpenSession (
  TEEC_Context     *Context,
  TEEC_Session     *Session,
  const TEEC_UUID  *Destination,
  uint32_t         Method,
  const void       *Data,
  TEEC_Operation   *Operation,
  uint32_t         *Origin
  )
{
  TestRequire (!TestAtEbs);
  *Origin = TEEC_ORIGIN_TRUSTED_APP;
  if (TestOpenResult == TEEC_SUCCESS) {
    Session->session_id = 7;
    Session->ctx        = Context;
  }

  return TestOpenResult;
}

TEEC_Result
OpteeClientCloseSession (
  TEEC_Session  *Session
  )
{
  TestCheckedCloses++;
  TestRequire (Session->session_id == 7);
  TestCloseSawReadinessRevoked = !mFtpmProtocol.GetFTPMStatus (&mFtpmProtocol);
  if (TestCloseResult == TEEC_SUCCESS) {
    memset (Session, 0, sizeof (*Session));
  }

  return TestCloseResult;
}

TEEC_Result
TEEC_InvokeCommand (
  TEEC_Session    *Session,
  uint32_t        Command,
  TEEC_Operation  *Operation,
  uint32_t        *Origin
  )
{
  TestRequire (Session->session_id == 7 && Command == 0);
  TestRequire (
               Operation->paramTypes == TEEC_PARAM_TYPES (
                                                          TEEC_MEMREF_TEMP_INPUT,
                                                          TEEC_MEMREF_TEMP_INOUT,
                                                          TEEC_NONE,
                                                          TEEC_NONE
                                                          )
               );
  TestInvocations++;
  TestSavedCommand      = Operation->params[0].tmpref.buffer;
  TestSavedResponse     = Operation->params[1].tmpref.buffer;
  TestSavedCommandSize  = Operation->params[0].tmpref.size;
  TestSavedResponseSize = Operation->params[1].tmpref.size;
  if (TestCheckZeroResponse) {
    TestRequire (TestSavedResponse != TestCallerOutput);
    TestRequire (TestAllZero (TestSavedResponse, TestSavedResponseSize));
  }

  // Script only the TA boundary: command serialization, the private bridge,
  // checked TA framing and both command parsers are production code.
  if (EFI_ERROR (TestInstallResult)) {
    STATIC CONST UINT8 PcrResponse[] = {
      0x80, 0x01, 0, 0, 0, 25, 0, 0, 0, 0,
      0, 0, 0, 0, 5, 0, 0, 0, 1, 0, 0x0b, 3, 0xff, 0xff, 0xff
    };
    STATIC CONST UINT8 ExtendResponse[] = {
      0x80, 0x02, 0, 0, 0, 19, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    STATIC CONST UINT8 ExpectedDigest[32] = {
      0x67, 0xab, 0xdd, 0x72, 0x10, 0x24, 0xf0, 0xff,
      0x4e, 0x0b, 0x3f, 0x4c, 0x2f, 0xc1, 0x3b, 0xc5,
      0xba, 0xd4, 0x2d, 0x0b, 0x78, 0x51, 0xd4, 0x56,
      0xd8, 0x8d, 0x20, 0x3d, 0x15, 0xaa, 0xa4, 0x50
    };
    UINT32 Code = SwapBytes32 (ReadUnaligned32 ((UINT32 *)(TestSavedCommand + 6)));
    TestRequire (!TestAtEbs && TestProtocol == NULL);
    TestRequire (FtpmGetStatus (&mFtpmProtocol));
    TestRequire (TestSavedResponse == mResponseBuffer);
    TestRequire (TestAllZero (TestSavedResponse, TestSavedResponseSize));
    if (Code == TPM_CC_GetCapability) {
      TestRequire (TestSavedCommandSize == 22);
      TestRequire (SwapBytes32 (ReadUnaligned32 ((UINT32 *)(TestSavedCommand + 10))) == TPM_CAP_PCRS);
      memcpy (TestSavedResponse, PcrResponse, sizeof (PcrResponse));
      Operation->params[1].tmpref.size = sizeof (PcrResponse);
      if (TestCapMalformedFrame) {
        TestSavedResponse[5]--;
      }
      *Origin = TEEC_ORIGIN_TRUSTED_APP;
      return TestCapDiscoveryResult;
    }
    TestRequire (Code == TPM_CC_PCR_Extend && TestSavedCommandSize == 65);
    TestRequire (SwapBytes32 (ReadUnaligned32 ((UINT32 *)(TestSavedCommand + 10))) == TestCapExtends);
    TestRequire (SwapBytes32 (ReadUnaligned32 ((UINT32 *)(TestSavedCommand + 14))) == 9);
    TestRequire (SwapBytes32 (ReadUnaligned32 ((UINT32 *)(TestSavedCommand + 18))) == TPM_RS_PW);
    TestRequire (TestAllZero (TestSavedCommand + 22, 5));
    TestRequire (SwapBytes32 (ReadUnaligned32 ((UINT32 *)(TestSavedCommand + 27))) == 1);
    TestRequire (SwapBytes16 (ReadUnaligned16 ((UINT16 *)(TestSavedCommand + 31))) == TPM_ALG_SHA256);
    TestRequire (memcmp (TestSavedCommand + 33, ExpectedDigest, sizeof (ExpectedDigest)) == 0);
    memcpy (TestSavedResponse, ExtendResponse, sizeof (ExtendResponse));
    Operation->params[1].tmpref.size = sizeof (ExtendResponse);
    if ((TestCapExtends == TestCapFailPcr) && EFI_ERROR (TestCapResult)) {
      TestSavedResponse[9] = 1;
    }
    TestCapExtends++;
    *Origin = TEEC_ORIGIN_TRUSTED_APP;
    return TEEC_SUCCESS;
  }

  memcpy (
          TestSavedResponse,
          TestResponse,
          TestResponseBytes < TestSavedResponseSize ?
          TestResponseBytes : TestSavedResponseSize
          );
  Operation->params[1].tmpref.size = TestReturnedSize;
  *Origin                          = TEEC_ORIGIN_TRUSTED_APP;
  return TestInvokeResult;
}


VOID
TestCapStoppedEntry (VOID)
{
  mExpectCapTerminal = TRUE;
  if (setjmp (mCapTerminal) == 0) {
    TestStart ();
  }
  mExpectCapTerminal = FALSE;
}

VOID EFIAPI
CpuDeadLoop (
  VOID
  )
{
  TestHalted = TRUE;
  if (mExpectCapTerminal) {
    longjmp (mCapTerminal, 1);
  }
  abort ();
}

BOOLEAN
TestPrivateReady (VOID)
{
  return FtpmGetStatus (&mFtpmProtocol);
}


STATIC EFI_STATUS EFIAPI
MockOutputString (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *Text)
{
  UINTN Index;
  UINTN Start = strlen (TestSerial);
  UINTN Length = 0;
  TestRequire (!TestAtEbs);
  while (Text[Length] != 0) {
    Length++;
  }
  TestRequire (Start >= Length);
  for (Index = 0; Index < Length; Index++) {
    TestRequire (Text[Index] == (UINT8)TestSerial[Start - Length + Index]);
  }
  TestConsoleWrites++;
  return EFI_SUCCESS;
}

UINTN EFIAPI
SerialPortWrite (UINT8 *Buffer, UINTN Size)
{
  UINTN Used = strlen (TestSerial);
  TestRequire (Used + Size < sizeof (TestSerial));
  memcpy (TestSerial + Used, Buffer, Size);
  TestSerial[Used + Size] = 0;
  return Size;
}

VOID
OpteeClientApiExitBootServices (VOID)
{
  // DXE Core signals BeforeExitBootServices before it accepts the map key.
  TestRequire (!TestAtEbs);
  TestSerialOnly = TRUE;
}

VOID
TestReset (
  VOID
  )
{
  memset (&mContext, 0, sizeof (mContext));
  memset (&mSession, 0, sizeof (mSession));
  mSessionOpen    = FALSE;
  mSessionUsable  = FALSE;
  mCommandBusy    = FALSE;
  memset (mResponseBuffer, 0, sizeof (mResponseBuffer));
  mClientStatus                = EFI_NOT_STARTED;
  mOpenResult                  = 0;
  mOpenOrigin                  = 0;
  mCommands                    = 0;
  mFailures                    = 0;
  mLastFailure                 = 0;
  mLastFailureOrigin           = 0;
  TestProtocol                 = NULL;
  mEbsCallback                 = NULL;
  mBeforeEbsCallback           = NULL;
  mBeforeEbsSignaled           = FALSE;
  TestAtEbs                    = FALSE;
  TestHalted                   = FALSE;
  TestInvokeResult             = TEEC_SUCCESS;
  TestCloseResult              = TEEC_SUCCESS;
  TestOpenResult               = TEEC_SUCCESS;
  TestEventResult              = EFI_SUCCESS;
  TestEventFailCall            = 0;
  TestEventCalls               = 0;
  TestSerialOnly               = FALSE;
  TestInstallResult            = EFI_SUCCESS;
  TestCloseSawReadinessRevoked = FALSE;
  TestLastDiagnosticCommands   = 0;
  TestLastDiagnosticFailures   = 0;
  TestInvocations              = 0;
  TestCheckedCloses            = 0;
  TestVariableWrites           = 0;
  TestCheckZeroResponse        = FALSE;
  TestSavedCommand             = NULL;
  TestSavedResponse            = NULL;
  TestCallerOutput             = NULL;
  memset (TestResponse, 0, sizeof (TestResponse));
  TestHeader (TestResponse, TPM_ST_NO_SESSIONS, 10);
  TestResponseBytes                               = 10;
  TestReturnedSize                                = 10;
  mBootServices.CreateEventEx                     = MockCreateEventEx;
  mBootServices.CloseEvent                        = MockCloseEvent;
  mBootServices.InstallMultipleProtocolInterfaces = MockInstall;
  mRuntimeServices.SetVariable                    = MockSetVariable;
  mReadyCallback = NULL;
  TestSerial[0] = 0;
  TestConsoleWrites = 0;
  TestClientResult = EFI_SUCCESS;
  TestReadyEventResult = EFI_SUCCESS;
  TestClosedEvents = 0;
  TestCapExtends = 0;
  TestCapResult = EFI_SUCCESS;
  TestCapDiscoveryResult = TEEC_SUCCESS;
  TestCapMalformedFrame = FALSE;
  TestCapFailPcr = 0;
  mExpectCapTerminal = FALSE;
  mConOut.OutputString = MockOutputString;
  mSystemTable.ConOut = &mConOut;
}

EFI_STATUS
TestStart (
  VOID
  )
{
  return FtpmDxeEntryPoint ((EFI_HANDLE)(UINTN)10, &mSystemTable);
}


/** DXE Core signals this group once, on the first ExitBootServices call. **/
VOID
TestBeforeExitBootServices (
  VOID
  )
{
  TestRequire (!TestAtEbs && mBeforeEbsCallback != NULL);
  if (!mBeforeEbsSignaled) {
    mBeforeEbsSignaled = TRUE;
    mBeforeEbsCallback ((EFI_EVENT)(UINTN)3, NULL);
  }
}

VOID
TestExitBootServices (
  VOID
  )
{
  if (!TestAtEbs) {
    TestBeforeExitBootServices ();
  }

  TestAtEbs = TRUE;
  TestRequire (mEbsCallback != NULL);
  mEbsCallback ((EFI_EVENT)(UINTN)1, NULL);
}

VOID
TestReadyToBoot (
  VOID
  )
{
  TestRequire (mReadyCallback != NULL);
  mReadyCallback (NULL, NULL);
}

BOOLEAN
TestSessionStillOpen (
  VOID
  )
{
  return mSessionOpen;
}
