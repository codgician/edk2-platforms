/** @file
  Compile production code unchanged; mock only its external services.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include "DriverHarness.h"
#include <Library/PrintLib.h>
#include <Library/UefiLib.h>

#include "../FTPMDxe.c"

EFI_GUID                     gOpteeFtpmTaGuid;
EFI_GUID                     gCixFtpmInitProtocolGuid = CIX_FTPM_INIT_PROTOCOL_GUID;
EFI_GUID                     gEfiEventExitBootServicesGuid;
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
BOOLEAN                  TestFinalizeResult;
BOOLEAN                  TestCloseSawReadinessRevoked;
UINT32                   TestLastDiagnosticCommands;
UINT32                   TestLastDiagnosticFailures;
EFI_STATUS               TestEventResult;
UINTN                    TestInvocations;
UINTN                    TestCheckedCloses;
UINTN                    TestLegacyCloses;
UINTN                    TestFinalizes;
UINTN                    TestVariableWrites;
BOOLEAN                  TestAtEbs;
BOOLEAN                  TestHalted;
BOOLEAN                  TestCheckZeroResponse;
BOOLEAN                  TestCheckPrivateCommand;
UINT8                    *TestCallerInput;
UINT8                    *TestCallerOutput;
UINT8                    *TestSavedCommand;
UINT8                    *TestSavedResponse;
UINTN                    TestSavedCommandSize;
UINTN                    TestSavedResponseSize;
STATIC jmp_buf           mTerminal;
STATIC BOOLEAN           mExpectTerminal;
STATIC EFI_EVENT_NOTIFY  mEbsCallback;

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
  TestRequire (!TestAtEbs && Tpl == TPL_CALLBACK && Type == EVT_NOTIFY_SIGNAL);
  TestRequire (Group == &gEfiEventExitBootServicesGuid);
  mEbsCallback = Notify;
  *Event       = (EFI_EVENT)(UINTN)1;
  return TestEventResult;
}

STATIC EFI_STATUS EFIAPI
MockCloseEvent (
  EFI_EVENT  Event
  )
{
  TestRequire (!TestAtEbs);
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
  return EFI_SUCCESS;
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
  *Event = (EFI_EVENT)(UINTN)2;
  return EFI_SUCCESS;
}

UINTN EFIAPI
AsciiSPrint (
  CHAR8        *Buffer,
  UINTN        Size,
  CONST CHAR8  *Format,
  ...
  )
{
  // Formatting itself is external PrintLib, not part of this test boundary.
  TestRequire (!TestAtEbs && Size >= 12);
  memcpy (Buffer, "diagnostic\n", 12);
  return 11;
}

EFI_STATUS
OpteeClientApiInitialize (
  EFI_HANDLE  ImageHandle
  )
{
  TestRequire (!TestAtEbs);
  return EFI_SUCCESS;
}

BOOLEAN
OpteeClientApiFinalize (
  VOID
  )
{
  TestRequire (!TestAtEbs);
  TestFinalizes++;
  return TestFinalizeResult;
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

void
TEEC_FinalizeContext (
  TEEC_Context  *Context
  )
{
  Context->fd = -1;
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

void
TEEC_CloseSession (
  TEEC_Session  *Session
  )
{
  TestLegacyCloses++;
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

  if (TestCheckPrivateCommand) {
    TestRequire (TestSavedCommand != TestCallerInput);
    TestRequire (memcmp (TestSavedCommand, TestCallerInput, TestSavedCommandSize) == 0);
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

VOID EFIAPI
CpuDeadLoop (
  VOID
  )
{
  TestRequire (mExpectTerminal);
  TestHalted = TRUE;
  longjmp (mTerminal, 1);
}

UINT16 EFIAPI
SwapBytes16 (
  UINT16  Value
  )
{
  return __builtin_bswap16 (Value);
}

UINT32 EFIAPI
SwapBytes32 (
  UINT32  Value
  )
{
  return __builtin_bswap32 (Value);
}

UINT16 EFIAPI
ReadUnaligned16 (
  CONST UINT16  *Buffer
  )
{
  CONST volatile UINT8  *Bytes = (CONST volatile UINT8 *)Buffer;

  return (UINT16)Bytes[0] | ((UINT16)Bytes[1] << 8);
}

UINT32 EFIAPI
ReadUnaligned32 (
  CONST UINT32  *Buffer
  )
{
  CONST volatile UINT8  *Bytes = (CONST volatile UINT8 *)Buffer;

  return (UINT32)Bytes[0] | ((UINT32)Bytes[1] << 8) |
         ((UINT32)Bytes[2] << 16) | ((UINT32)Bytes[3] << 24);
}

VOID
TestReset (
  VOID
  )
{
  memset (&mContext, 0, sizeof (mContext));
  memset (&mSession, 0, sizeof (mSession));
  mClientReady    = FALSE;
  mSessionOpen    = FALSE;
  mSessionUsable  = FALSE;
  mHandoffStarted = FALSE;
  mCommandBusy    = FALSE;
  memset (mCommandBuffer, 0, sizeof (mCommandBuffer));
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
  TestAtEbs                    = FALSE;
  TestHalted                   = FALSE;
  TestInvokeResult             = TEEC_SUCCESS;
  TestCloseResult              = TEEC_SUCCESS;
  TestOpenResult               = TEEC_SUCCESS;
  TestEventResult              = EFI_SUCCESS;
  TestFinalizeResult           = TRUE;
  TestCloseSawReadinessRevoked = FALSE;
  TestLastDiagnosticCommands   = 0;
  TestLastDiagnosticFailures   = 0;
  TestInvocations              = 0;
  TestCheckedCloses            = 0;
  TestLegacyCloses             = 0;
  TestFinalizes                = 0;
  TestVariableWrites           = 0;
  TestCheckZeroResponse        = FALSE;
  TestCheckPrivateCommand      = FALSE;
  TestSavedCommand             = NULL;
  TestSavedResponse            = NULL;
  TestCallerInput              = NULL;
  TestCallerOutput             = NULL;
  memset (TestResponse, 0, sizeof (TestResponse));
  TestHeader (TestResponse, TPM_ST_NO_SESSIONS, 10);
  TestResponseBytes                               = 10;
  TestReturnedSize                                = 10;
  mBootServices.CreateEventEx                     = MockCreateEventEx;
  mBootServices.CloseEvent                        = MockCloseEvent;
  mBootServices.InstallMultipleProtocolInterfaces = MockInstall;
  mRuntimeServices.SetVariable                    = MockSetVariable;
}

EFI_STATUS
TestStart (
  VOID
  )
{
  return FtpmDxeEntryPoint (NULL, NULL);
}

VOID
TestExitBootServices (
  VOID
  )
{
  TestAtEbs = TRUE;
  TestRequire (mEbsCallback != NULL);
  mEbsCallback ((EFI_EVENT)(UINTN)1, NULL);
}

VOID
TestExpectTerminalExit (
  VOID
  )
{
  mExpectTerminal = TRUE;
  if (setjmp (mTerminal) == 0) {
    TestExitBootServices ();
  }

  mExpectTerminal = FALSE;
}

VOID
TestReadyToBoot (
  VOID
  )
{
  OnReadyToBoot (NULL, NULL);
}

BOOLEAN
TestSessionStillOpen (
  VOID
  )
{
  return mSessionOpen;
}
