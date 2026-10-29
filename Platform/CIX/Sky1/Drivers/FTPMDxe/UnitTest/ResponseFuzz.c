/** @file
  Feed TA response bytes/status/size through production SubmitCommand.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "DriverHarness.h"
#include <IndustryStandard/Tpm20.h>

int
LLVMFuzzerTestOneInput (
  const uint8_t  *Data,
  size_t         Size
  )
{
  if (Size < 14) {
    return 0;
  }

  TestReset ();
  TestRequire (TestStart () == EFI_SUCCESS);
  UINT8  Command[10];
  UINT8  Output[TEST_TPM_MAX + 2];
  TestHeader (Command, TPM_ST_NO_SESSIONS, sizeof (Command));
  memset (Output, 0xA5, sizeof (Output));
  uint64_t  Reported = 0;
  memcpy (&Reported, Data, sizeof (Reported));
  UINT32  Capacity         = ((UINT32)Data[8] | ((UINT32)Data[9] << 8)) % (TEST_TPM_MAX + 1);
  UINT32  OriginalCapacity = Capacity;
  switch (Data[10] % 4) {
    case 0: TestInvokeResult = TEEC_SUCCESS;
      break;
    case 1: TestInvokeResult = TEEC_ERROR_SHORT_BUFFER;
      break;
    case 2: TestInvokeResult = TEEC_ERROR_TARGET_DEAD;
      break;
    default: TestInvokeResult = TEEC_ERROR_GENERIC;
      break;
  }

  TestResponseBytes = Size - 14 < TEST_TPM_MAX ? Size - 14 : TEST_TPM_MAX;
  memset (TestResponse, 0, sizeof (TestResponse));
  memcpy (TestResponse, Data + 14, TestResponseBytes);
  // Structured modes keep valid frames and exact boundary cases reachable.
  if (((Data[11] & 1) != 0) && (TestResponseBytes >= 10)) {
    TestHeader (
                TestResponse,
                (Data[12] & 1) ? TPM_ST_SESSIONS : TPM_ST_NO_SESSIONS,
                (UINT32)TestResponseBytes
                );
  }

  if ((Data[11] & 2) != 0) {
    Reported = TestResponseBytes;
  }

  TestReturnedSize        = Reported;
  TestCallerInput         = Command;
  TestCallerOutput        = Output + 1;
  TestCheckZeroResponse   = TRUE;
  TestCheckPrivateCommand = TRUE;
  EFI_STATUS  Status     = TestProtocol->SubmitCommand (TestProtocol, sizeof (Command), Command, &Capacity, Output + 1);
  UINT32      HeaderSize = ((UINT32)TestResponse[2] << 24) | ((UINT32)TestResponse[3] << 16) |
                           ((UINT32)TestResponse[4] << 8) | TestResponse[5];
  UINT16   Tag   = ((UINT16)TestResponse[0] << 8) | TestResponse[1];
  BOOLEAN  Valid = Reported >= 10 && Reported <= OriginalCapacity &&
                   HeaderSize == Reported && (Tag == TPM_ST_SESSIONS || Tag == TPM_ST_NO_SESSIONS);
  if ((TestInvokeResult == TEEC_SUCCESS) && Valid) {
    TestRequire (Status == EFI_SUCCESS && Capacity == Reported);
    TestRequire (memcmp (Output + 1, TestResponse, Capacity) == 0);
  } else if ((TestInvokeResult == TEEC_ERROR_SHORT_BUFFER) && (Reported > OriginalCapacity) &&
             (Reported >= 10) && (Reported <= TEST_TPM_MAX))
  {
    TestRequire (Status == EFI_BUFFER_TOO_SMALL && Capacity == Reported);
  } else {
    TestRequire (Status == EFI_DEVICE_ERROR && Capacity == 0);
  }

  if (Status != EFI_SUCCESS) {
    TestRequire (Output[1] == 0xA5);
  }

  TestRequire (Output[0] == 0xA5 && Output[OriginalCapacity + 1] == 0xA5);
  TestRequire (TestAllZero (TestSavedCommand, TEST_TPM_MAX));
  TestRequire (TestAllZero (TestSavedResponse, TEST_TPM_MAX));
  return 0;
}
