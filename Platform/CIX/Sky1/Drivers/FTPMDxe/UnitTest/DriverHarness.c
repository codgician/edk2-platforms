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
#include <Protocol/LoadedImage.h>
#include <Protocol/DevicePath.h>
#include <Library/SerialPortLib.h>

#include "../FTPMDxe.c"

EFI_GUID                     gOpteeFtpmTaGuid;
EFI_GUID                     gCixFtpmInitProtocolGuid = CIX_FTPM_INIT_PROTOCOL_GUID;
EFI_GUID                     gEfiEventBeforeExitBootServicesGuid;
EFI_GUID                     gEfiEventExitBootServicesGuid;
EFI_GUID                     gEfiLoadedImageProtocolGuid;
EFI_GUID                     gEfiLoadedImageDevicePathProtocolGuid;
EFI_GUID                     gEfiDevicePathProtocolGuid;
STATIC EFI_SYSTEM_TABLE      mSystemTable;
EFI_SYSTEM_TABLE             *gST = &mSystemTable;
STATIC EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL mConOut;
STATIC EFI_LOADED_IMAGE_PROTOCOL mParentImage;
STATIC EFI_LOADED_IMAGE_PROTOCOL mChildImage;
STATIC EFI_DEVICE_PATH_PROTOCOL mImagePath = { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { 4, 0 } };
STATIC BOOLEAN               mInsideChild;
STATIC BOOLEAN               mEbsResident;
STATIC BOOLEAN               mReadyResident;
STATIC EFI_EVENT_NOTIFY      mReadyCallback;
CHAR8                        TestSerial[4096];
UINTN                        TestConsoleWrites;
EFI_STATUS                   TestClientResult;
TEEC_Result                  TestContextResult;
EFI_STATUS                   TestReadyEventResult;
EFI_STATUS                   TestLoadResult;
EFI_STATUS                   TestStartResult;
UINTN                        TestResidentImages;
UINTN                        TestClosedEvents;
UINTN                        TestContextFinalizes;
UINTN                        TestCapExtends;
EFI_STATUS                   TestCapResult;
TEEC_Result                  TestCapDiscoveryResult;
BOOLEAN                      TestCapMalformedFrame;
BOOLEAN                      TestCapMalformedExtend;
BOOLEAN                      TestCapMoreData;
UINTN                        TestCapFailPcr;
STATIC jmp_buf               mCapTerminal;
STATIC BOOLEAN               mExpectCapTerminal;
STATIC EFI_BOOT_SERVICES     mBootServices;
STATIC EFI_RUNTIME_SERVICES  mRuntimeServices;
EFI_BOOT_SERVICES            *gBS = &mBootServices;
EFI_RUNTIME_SERVICES         *gRT = &mRuntimeServices;

VOID
TestWithoutConsole (VOID)
{
  mSystemTable.ConOut = NULL;
}

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
UINTN                    TestEventFailCall;
UINTN                    TestEventCalls;
BOOLEAN                  TestSerialOnly;
UINTN                    TestInvocations;
EFI_STATUS               TestInstallResult;
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
STATIC EFI_EVENT_NOTIFY  mEbsCallback;
STATIC EFI_EVENT_NOTIFY  mBeforeEbsCallback;
STATIC BOOLEAN           mBeforeEbsResident;
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
    mBeforeEbsResident = mInsideChild;
    if (!EFI_ERROR (Status)) {
      mBeforeEbsCallback = Notify;
      *Event             = (EFI_EVENT)(UINTN)3;
    }

    return Status;
  }

  mEbsResident = mInsideChild;
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
  TestRequire (!TestAtEbs);
  TestClosedEvents++;
  if ((UINTN)Event == 1) {
    mEbsCallback = NULL;
  } else if ((UINTN)Event == 3) {
    mBeforeEbsCallback = NULL;
  } else {
    TestRequire ((UINTN)Event == 2);
    mReadyCallback = NULL;
  }
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
    mReadyResident = mInsideChild;
    *Event = (EFI_EVENT)(UINTN)2;
  }
  return TestReadyEventResult;
}

UINT64 EFIAPI
DivU64x32Remainder (UINT64 Dividend, UINT32 Divisor, UINT32 *Remainder)
{
  if (Remainder != NULL) {
    *Remainder = Dividend % Divisor;
  }

  return Dividend / Divisor;
}

UINTN EFIAPI
StrnLenS (CONST CHAR16 *String, UINTN MaxSize)
{
  UINTN  Length;

  for (Length = 0; (Length < MaxSize) && (String[Length] != 0); Length++) {
  }

  return Length;
}

EFI_STATUS
OpteeClientApiInitialize (
  EFI_HANDLE  ImageHandle
  )
{
  TestRequire (!TestAtEbs);
  return TestClientResult;
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
  return TestContextResult;
}

void
TEEC_FinalizeContext (
  TEEC_Context  *Context
  )
{
  TestRequire (!TestAtEbs);
  TestContextFinalizes++;
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
    TestRequire (!TestAtEbs && mInsideChild && TestProtocol == NULL);
    TestRequire (FtpmResidentReady ());
    TestRequire (TestSavedCommand == mCommandBuffer && TestSavedResponse == mResponseBuffer);
    TestRequire (TestAllZero (TestSavedResponse, TestSavedResponseSize));
    if (Code == TPM_CC_GetCapability) {
      TestRequire (TestSavedCommandSize == 22);
      TestRequire (SwapBytes32 (ReadUnaligned32 ((UINT32 *)(TestSavedCommand + 10))) == TPM_CAP_PCRS);
      memcpy (TestSavedResponse, PcrResponse, sizeof (PcrResponse));
      TestSavedResponse[10] = TestCapMoreData ? 1 : 0;
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
    if (TestCapMalformedExtend) {
      TestHeader (TestSavedResponse, TPM_ST_NO_SESSIONS, 10);
      Operation->params[1].tmpref.size = 10;
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
UINT16 EFIAPI
WriteUnaligned16 (UINT16 *Buffer, UINT16 Value)
{
  volatile UINT8  *Bytes = (volatile UINT8 *)Buffer;

  Bytes[0] = (UINT8)Value;
  Bytes[1] = (UINT8)(Value >> 8);
  return Value;
}

UINT32 EFIAPI
WriteUnaligned32 (UINT32 *Buffer, UINT32 Value)
{
  volatile UINT8  *Bytes = (volatile UINT8 *)Buffer;

  Bytes[0] = (UINT8)Value;
  Bytes[1] = (UINT8)(Value >> 8);
  Bytes[2] = (UINT8)(Value >> 16);
  Bytes[3] = (UINT8)(Value >> 24);
  return Value;
}

BOOLEAN EFIAPI
IsZeroBuffer (CONST VOID *Buffer, UINTN Length)
{
  return TestAllZero (Buffer, Length);
}

BOOLEAN
TestPrivateReady (VOID)
{
  return FtpmResidentReady ();
}


UINTN EFIAPI
GetDevicePathSize (CONST EFI_DEVICE_PATH_PROTOCOL *Path)
{
  TestRequire (Path == &mImagePath);
  return sizeof (mImagePath);
}

STATIC EFI_STATUS EFIAPI
MockHandleProtocol (EFI_HANDLE Handle, EFI_GUID *Guid, VOID **Interface)
{
  TestRequire (!TestAtEbs);
  TestRequire ((UINTN)Handle == 10 || (UINTN)Handle == 11);
  if (Guid == &gEfiLoadedImageProtocolGuid) {
    *Interface = (UINTN)Handle == 10 ? &mParentImage : &mChildImage;
  } else {
    TestRequire (Guid == &gEfiLoadedImageDevicePathProtocolGuid);
    *Interface = &mImagePath;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
MockLoadImage (BOOLEAN Policy, EFI_HANDLE Parent, EFI_DEVICE_PATH_PROTOCOL *Path,
               VOID *Buffer, UINTN Size, EFI_HANDLE *Handle)
{
  TestRequire (!TestAtEbs && !mInsideChild && !Policy);
  TestRequire ((UINTN)Parent == 10 && Path == &mImagePath && Buffer == NULL && Size == 0);
  if (!EFI_ERROR (TestLoadResult)) {
    *Handle = (EFI_HANDLE)(UINTN)11;
  }
  return TestLoadResult;
}

STATIC EFI_STATUS EFIAPI
MockStartImage (EFI_HANDLE Handle, UINTN *Size, CHAR16 **Data)
{
  EFI_STATUS Status;
  TestRequire (!TestAtEbs && !mInsideChild && (UINTN)Handle == 11);
  if (EFI_ERROR (TestStartResult)) {
    memset (&mChildImage, 0, sizeof (mChildImage));
    return TestStartResult;
  }
  mInsideChild = TRUE;
  Status = FtpmDxeEntryPoint (Handle, &mSystemTable);
  mInsideChild = FALSE;
  if (!EFI_ERROR (Status)) {
    TestResidentImages++;
  } else {
    TestRequire (mEbsCallback == NULL && mReadyCallback == NULL && mBeforeEbsCallback == NULL);
  }
  return Status;
}

STATIC EFI_STATUS EFIAPI
MockUnloadImage (EFI_HANDLE Handle)
{
  TestRequire (!TestAtEbs && (UINTN)Handle == 11);
  return TestResidentImages != 0 && mChildImage.Unload == NULL ? EFI_UNSUPPORTED : EFI_SUCCESS;
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

BOOLEAN
TestCallbacksResident (VOID)
{
  return TestResidentImages == 1 && (mEbsCallback == NULL || mEbsResident) &&
         (mBeforeEbsCallback == NULL || mBeforeEbsResident) &&
         (mReadyCallback == NULL || mReadyResident);
}

BOOLEAN
TestHandshakeCleared (VOID)
{
  return mChildImage.LoadOptions == NULL && mChildImage.LoadOptionsSize == 0 &&
         mParentImage.LoadOptions == NULL && mParentImage.LoadOptionsSize == 0;
}

EFI_STATUS
TestUnloadResident (VOID)
{
  return MockUnloadImage ((EFI_HANDLE)(UINTN)11);
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
  memset (&mParentImage, 0, sizeof (mParentImage));
  memset (&mChildImage, 0, sizeof (mChildImage));
  mParentImage.FilePath = &mImagePath;
  mChildImage.FilePath = &mImagePath;
  mChildImage.ParentHandle = (EFI_HANDLE)(UINTN)10;
  mInsideChild = FALSE;
  mReadyCallback = NULL;
  TestSerial[0] = 0;
  TestConsoleWrites = 0;
  TestClientResult = EFI_SUCCESS;
  TestContextResult = TEEC_SUCCESS;
  TestReadyEventResult = EFI_SUCCESS;
  TestLoadResult = EFI_SUCCESS;
  TestStartResult = EFI_SUCCESS;
  TestResidentImages = 0;
  TestClosedEvents = 0;
  TestContextFinalizes = 0;
  TestCapExtends = 0;
  TestCapResult = EFI_SUCCESS;
  TestCapDiscoveryResult = TEEC_SUCCESS;
  TestCapMalformedFrame = FALSE;
  TestCapMalformedExtend = FALSE;
  TestCapMoreData = FALSE;
  TestCapFailPcr = 0;
  mExpectCapTerminal = FALSE;
  mBootServices.HandleProtocol = MockHandleProtocol;
  mBootServices.LoadImage = MockLoadImage;
  mBootServices.StartImage = MockStartImage;
  mBootServices.UnloadImage = MockUnloadImage;
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
