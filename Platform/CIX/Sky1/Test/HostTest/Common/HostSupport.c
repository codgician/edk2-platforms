/** @file
  Host implementations of the EDK2 base services production code links:
  BaseMemoryLib, MemoryAllocationLib, DebugLib, TimerLib and the BaseLib
  string helpers in use.

  Pool allocations follow EDK2 semantics (a zero-size request returns a valid
  pointer) and are counted so tests can check for leaks across a scenario.
  ASSERT aborts the test.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>

#include "HostSupport.h"

STATIC INTN  mLivePoolAllocations;

INTN
HostLivePoolAllocations (
  VOID
  )
{
  return mLivePoolAllocations;
}

//
// BaseMemoryLib
//
VOID *
EFIAPI
CopyMem (
  OUT VOID       *Destination,
  IN CONST VOID  *Source,
  IN UINTN       Length
  )
{
  if (Length != 0) {
    memmove (Destination, Source, Length);
  }

  return Destination;
}

VOID *
EFIAPI
SetMem (
  OUT VOID  *Buffer,
  IN UINTN  Length,
  IN UINT8  Value
  )
{
  if (Length != 0) {
    memset (Buffer, Value, Length);
  }

  return Buffer;
}

VOID *
EFIAPI
ZeroMem (
  OUT VOID  *Buffer,
  IN UINTN  Length
  )
{
  return SetMem (Buffer, Length, 0);
}

INTN
EFIAPI
CompareMem (
  IN CONST VOID  *DestinationBuffer,
  IN CONST VOID  *SourceBuffer,
  IN UINTN       Length
  )
{
  return (Length == 0) ? 0 : memcmp (DestinationBuffer, SourceBuffer, Length);
}

GUID *
EFIAPI
CopyGuid (
  OUT GUID       *DestinationGuid,
  IN CONST GUID  *SourceGuid
  )
{
  return CopyMem (DestinationGuid, SourceGuid, sizeof (GUID));
}

BOOLEAN
EFIAPI
CompareGuid (
  IN CONST GUID  *Guid1,
  IN CONST GUID  *Guid2
  )
{
  return CompareMem (Guid1, Guid2, sizeof (GUID)) == 0;
}

//
// MemoryAllocationLib (pool only; tests model page allocations themselves)
//
VOID *
EFIAPI
AllocatePool (
  IN UINTN  AllocationSize
  )
{
  VOID  *Buffer;

  Buffer = malloc (AllocationSize == 0 ? 1 : AllocationSize);
  if (Buffer != NULL) {
    mLivePoolAllocations++;
  }

  return Buffer;
}

VOID *
EFIAPI
AllocateZeroPool (
  IN UINTN  AllocationSize
  )
{
  VOID  *Buffer;

  Buffer = AllocatePool (AllocationSize);
  if (Buffer != NULL) {
    memset (Buffer, 0, AllocationSize);
  }

  return Buffer;
}

VOID *
EFIAPI
AllocateCopyPool (
  IN UINTN       AllocationSize,
  IN CONST VOID  *Buffer
  )
{
  VOID  *Copy;

  Copy = AllocatePool (AllocationSize);
  if (Copy != NULL) {
    CopyMem (Copy, Buffer, AllocationSize);
  }

  return Copy;
}

VOID *
EFIAPI
ReallocatePool (
  IN UINTN  OldSize,
  IN UINTN  NewSize,
  IN VOID   *OldBuffer  OPTIONAL
  )
{
  VOID  *NewBuffer;

  NewBuffer = AllocateZeroPool (NewSize);
  if ((NewBuffer != NULL) && (OldBuffer != NULL)) {
    CopyMem (NewBuffer, OldBuffer, MIN (OldSize, NewSize));
    FreePool (OldBuffer);
  }

  return NewBuffer;
}

VOID
EFIAPI
FreePool (
  IN VOID  *Buffer
  )
{
  if (Buffer == NULL) {
    fprintf (stderr, "FreePool(NULL)\n");
    abort ();
  }

  mLivePoolAllocations--;
  free (Buffer);
}

//
// DebugLib
//
VOID
EFIAPI
DebugPrint (
  IN UINTN        ErrorLevel,
  IN CONST CHAR8  *Format,
  ...
  )
{
  (VOID)ErrorLevel;
  (VOID)Format;
}

VOID
EFIAPI
DebugAssert (
  IN CONST CHAR8  *FileName,
  IN UINTN        LineNumber,
  IN CONST CHAR8  *Description
  )
{
  fprintf (stderr, "ASSERT %s(%lu): %s\n", FileName, (unsigned long)LineNumber, Description);
  abort ();
}

VOID *
EFIAPI
DebugClearMemory (
  OUT VOID  *Buffer,
  IN UINTN  Length
  )
{
  return SetMem (Buffer, Length, 0xAF);
}

BOOLEAN
EFIAPI
DebugAssertEnabled (
  VOID
  )
{
  return TRUE;
}

BOOLEAN
EFIAPI
DebugPrintEnabled (
  VOID
  )
{
  return FALSE;
}

BOOLEAN
EFIAPI
DebugCodeEnabled (
  VOID
  )
{
  return FALSE;
}

BOOLEAN
EFIAPI
DebugClearMemoryEnabled (
  VOID
  )
{
  return FALSE;
}

BOOLEAN
EFIAPI
DebugPrintLevelEnabled (
  IN CONST UINTN  ErrorLevel
  )
{
  (VOID)ErrorLevel;
  return FALSE;
}

//
// TimerLib
//
UINTN
EFIAPI
MicroSecondDelay (
  IN UINTN  MicroSeconds
  )
{
  return MicroSeconds;
}

//
// BaseLib string helpers
//
UINTN
EFIAPI
AsciiStrLen (
  IN CONST CHAR8  *String
  )
{
  return strlen (String);
}

UINTN
EFIAPI
AsciiStrnLenS (
  IN CONST CHAR8  *String,
  IN UINTN        MaxSize
  )
{
  return (String == NULL) ? 0 : strnlen (String, MaxSize);
}

INTN
EFIAPI
AsciiStrCmp (
  IN CONST CHAR8  *FirstString,
  IN CONST CHAR8  *SecondString
  )
{
  return strcmp (FirstString, SecondString);
}

RETURN_STATUS
EFIAPI
AsciiStrCpyS (
  OUT CHAR8        *Destination,
  IN  UINTN        DestMax,
  IN  CONST CHAR8  *Source
  )
{
  UINTN  Length;

  if ((Destination == NULL) || (Source == NULL)) {
    return RETURN_INVALID_PARAMETER;
  }

  Length = strnlen (Source, DestMax);
  if ((DestMax == 0) || (Length >= DestMax)) {
    return RETURN_BUFFER_TOO_SMALL;
  }

  memcpy (Destination, Source, Length + 1);
  return RETURN_SUCCESS;
}
