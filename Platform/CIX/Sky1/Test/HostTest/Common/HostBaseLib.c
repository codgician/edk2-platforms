/** @file
  Host implementations of the pure BaseLib helpers production code links.

  The unaligned accessors stay byte-wise: MdePkg's Unaligned.c dereferences
  the pointer directly, which UBSan rejects at odd TPM-frame offsets.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/BaseLib.h>

UINT16
EFIAPI
SwapBytes16 (
  IN UINT16  Value
  )
{
  return __builtin_bswap16 (Value);
}

UINT32
EFIAPI
SwapBytes32 (
  IN UINT32  Value
  )
{
  return __builtin_bswap32 (Value);
}

UINT16
EFIAPI
ReadUnaligned16 (
  IN CONST UINT16  *Buffer
  )
{
  CONST volatile UINT8  *Bytes = (CONST volatile UINT8 *)Buffer;

  return (UINT16)(Bytes[0] | ((UINT16)Bytes[1] << 8));
}

UINT32
EFIAPI
ReadUnaligned32 (
  IN CONST UINT32  *Buffer
  )
{
  return ReadUnaligned16 ((CONST UINT16 *)Buffer) |
         ((UINT32)ReadUnaligned16 ((CONST UINT16 *)Buffer + 1) << 16);
}

UINT64
EFIAPI
ReadUnaligned64 (
  IN CONST UINT64  *Buffer
  )
{
  return ReadUnaligned32 ((CONST UINT32 *)Buffer) |
         ((UINT64)ReadUnaligned32 ((CONST UINT32 *)Buffer + 1) << 32);
}

UINT16
EFIAPI
WriteUnaligned16 (
  OUT UINT16  *Buffer,
  IN  UINT16  Value
  )
{
  volatile UINT8  *Bytes = (volatile UINT8 *)Buffer;

  Bytes[0] = (UINT8)Value;
  Bytes[1] = (UINT8)(Value >> 8);
  return Value;
}

UINT32
EFIAPI
WriteUnaligned32 (
  OUT UINT32  *Buffer,
  IN  UINT32  Value
  )
{
  WriteUnaligned16 ((UINT16 *)Buffer, (UINT16)Value);
  WriteUnaligned16 ((UINT16 *)Buffer + 1, (UINT16)(Value >> 16));
  return Value;
}

UINT64
EFIAPI
WriteUnaligned64 (
  OUT UINT64  *Buffer,
  IN  UINT64  Value
  )
{
  WriteUnaligned32 ((UINT32 *)Buffer, (UINT32)Value);
  WriteUnaligned32 ((UINT32 *)Buffer + 1, (UINT32)(Value >> 32));
  return Value;
}

BOOLEAN
EFIAPI
IsZeroBuffer (
  IN CONST VOID  *Buffer,
  IN UINTN       Length
  )
{
  CONST UINT8  *Bytes = Buffer;

  while (Length-- != 0) {
    if (*Bytes++ != 0) {
      return FALSE;
    }
  }

  return TRUE;
}

BOOLEAN
EFIAPI
IsZeroGuid (
  IN CONST GUID  *Guid
  )
{
  return IsZeroBuffer (Guid, sizeof (GUID));
}

UINT8
EFIAPI
CalculateSum8 (
  IN CONST UINT8  *Buffer,
  IN UINTN        Length
  )
{
  UINT8  Sum = 0;

  while (Length-- != 0) {
    Sum = (UINT8)(Sum + *Buffer++);
  }

  return Sum;
}

UINT8
EFIAPI
CalculateCheckSum8 (
  IN CONST UINT8  *Buffer,
  IN UINTN        Length
  )
{
  return (UINT8)(0x100 - CalculateSum8 (Buffer, Length));
}

UINTN
EFIAPI
StrnLenS (
  IN CONST CHAR16  *String,
  IN UINTN         MaxSize
  )
{
  UINTN  Length;

  if (String == NULL) {
    return 0;
  }

  for (Length = 0; (Length < MaxSize) && (String[Length] != 0); Length++) {
  }

  return Length;
}

UINTN
EFIAPI
StrLen (
  IN CONST CHAR16  *String
  )
{
  return StrnLenS (String, MAX_UINTN);
}

UINTN
EFIAPI
StrSize (
  IN CONST CHAR16  *String
  )
{
  return (StrLen (String) + 1) * sizeof (CHAR16);
}

INTN
EFIAPI
StrCmp (
  IN CONST CHAR16  *FirstString,
  IN CONST CHAR16  *SecondString
  )
{
  while ((*FirstString != 0) && (*FirstString == *SecondString)) {
    FirstString++;
    SecondString++;
  }

  return *FirstString - *SecondString;
}

UINT64
EFIAPI
DivU64x32Remainder (
  IN  UINT64  Dividend,
  IN  UINT32  Divisor,
  OUT UINT32  *Remainder  OPTIONAL
  )
{
  if (Remainder != NULL) {
    *Remainder = (UINT32)(Dividend % Divisor);
  }

  return Dividend / Divisor;
}

UINT64
EFIAPI
LShiftU64 (
  IN UINT64  Operand,
  IN UINTN   Count
  )
{
  return Operand << Count;
}
