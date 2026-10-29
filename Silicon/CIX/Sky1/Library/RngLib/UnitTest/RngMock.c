/** SPDX-License-Identifier: BSD-2-Clause-Patent **/
#include <stdlib.h>
#include <string.h>
#include <Library/BaseMemoryLib.h>
#include "RngMock.h"

RNG_REPLY gReply;
UINTN gExpectedLength;
UINTN gMailboxCalls;
UINTN gResponseScrubs;
BOOLEAN gCheckSentinel;
STATIC VOID *mResponse;

#define REQUIRE(Expression) do { if (!(Expression)) { abort (); } } while (0)

VOID
ResetRngMock (UINTN Length)
{
  UINTN Index;
  memset (&gReply, 0, sizeof (gReply));
  gReply.Length = Length;
  for (Index = 0; Index < sizeof (gReply.Data); Index++) {
    gReply.Data[Index] = (UINT8)(Index + 1);
  }
  gExpectedLength = Length;
  gMailboxCalls = 0;
  gResponseScrubs = 0;
  gCheckSentinel = FALSE;
  mResponse = NULL;
}

EFI_STATUS
EFIAPI
MboxGetTrng (MBOX_TRNG_PARAMETER *Params, MBOX_TRNG_RESPONSE *Response)
{
  REQUIRE ((Params != NULL) && (Response != NULL));
  REQUIRE ((gExpectedLength != 0) && (gExpectedLength <= MAX_TRNG_SIZE));
  REQUIRE ((gMailboxCalls == 0) && (Params->TrngLen == gExpectedLength));
  gMailboxCalls++;
  mResponse = Response;
  if (gCheckSentinel) {
    UINTN Index;
    REQUIRE ((Response->ErrCode != 0) && (Response->TrngLen == 0));
    for (Index = 0; Index < sizeof (Response->Trng); Index++) {
      REQUIRE (Response->Trng[Index] == 0);
    }
  }
  if (!gReply.Unchanged) {
    Response->ErrCode = gReply.Error;
    Response->TrngLen = gReply.Length;
    memcpy (Response->Trng, gReply.Data, sizeof (Response->Trng));
  }
  return gReply.Status;
}

VOID *EFIAPI __real_ZeroMem (VOID *Buffer, UINTN Length);
VOID *EFIAPI
__wrap_ZeroMem (VOID *Buffer, UINTN Length)
{
  VOID *Result;
  UINTN Index;
  Result = __real_ZeroMem (Buffer, Length);
  if ((mResponse != NULL) && (Buffer == mResponse) && (Length == sizeof (MBOX_TRNG_RESPONSE))) {
    for (Index = 0; Index < Length; Index++) {
      REQUIRE (((UINT8 *)Buffer)[Index] == 0);
    }
    gResponseScrubs++;
    mResponse = NULL;
  }
  return Result;
}

VOID
CheckRngMock (BOOLEAN ExpectScrub)
{
  REQUIRE (gMailboxCalls == 1);
  if (ExpectScrub) {
    REQUIRE ((gResponseScrubs == 1) && (mResponse == NULL));
  }
  mResponse = NULL;
}
