/** SPDX-License-Identifier: BSD-2-Clause-Patent **/
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <Library/RngLib.h>
#include "RngMock.h"

int
LLVMFuzzerTestOneInput (const uint8_t *Data, size_t Size)
{
  UINT64 Buffer[4];
  UINT8 Before[sizeof (Buffer)];
  UINTN Width;
  UINTN Count;
  BOOLEAN Expected;
  BOOLEAN Result;
  if (Size < 10) {
    return 0;
  }
  Width = (UINTN)2 << (Data[0] & 3);
  ResetRngMock (Width);
  gReply.Unchanged = (Data[1] & 1) != 0;
  gReply.Status = (Data[1] & 2) != 0 ? EFI_DEVICE_ERROR : EFI_SUCCESS;
  memcpy (&gReply.Error, Data + 2, 4);
  memcpy (&gReply.Length, Data + 6, 4);
  Count = Size - 10;
  if (Count > sizeof (gReply.Data)) {
    Count = sizeof (gReply.Data);
  }
  memcpy (gReply.Data, Data + 10, Count);
  gCheckSentinel = TRUE;
  Expected = !gReply.Unchanged && !EFI_ERROR (gReply.Status) &&
             (gReply.Error == 0) && (gReply.Length == Width);
  memset (Buffer, 0xA5, sizeof (Buffer));
  memcpy (Before, Buffer, sizeof (Buffer));
  switch (Width) {
    case 2: Result = GetRandomNumber16 ((UINT16 *)&Buffer[1]); break;
    case 4: Result = GetRandomNumber32 ((UINT32 *)&Buffer[1]); break;
    case 8: Result = GetRandomNumber64 (&Buffer[1]); break;
    default: Result = GetRandomNumber128 (&Buffer[1]); break;
  }
  if ((Result != Expected) ||
      (memcmp (Buffer, Before, 8) != 0) ||
      (memcmp ((UINT8 *)&Buffer[1] + Width, Before + 8 + Width, sizeof (Buffer) - 8 - Width) != 0) ||
      (memcmp (&Buffer[1], Expected ? gReply.Data : Before + 8, Width) != 0)) {
    abort ();
  }
  CheckRngMock (TRUE);
  return 0;
}
