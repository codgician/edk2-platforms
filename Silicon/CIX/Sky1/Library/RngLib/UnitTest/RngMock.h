/** SPDX-License-Identifier: BSD-2-Clause-Patent **/
#ifndef RNG_MOCK_H_
#define RNG_MOCK_H_
#include <Uefi.h>
#include <Library/MailBoxLib.h>

typedef struct {
  EFI_STATUS Status;
  UINT32 Error;
  UINT32 Length;
  UINT8 Data[MAX_TRNG_SIZE];
  BOOLEAN Unchanged;
} RNG_REPLY;

extern RNG_REPLY gReply;
extern UINTN gExpectedLength;
extern UINTN gMailboxCalls;
extern BOOLEAN gCheckSentinel;
VOID ResetRngMock (UINTN Length);
VOID CheckRngMock (VOID);
#endif
