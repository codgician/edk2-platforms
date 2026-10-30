/** @file

  Copyright 2024 Cix Technology Group Co., Ltd. All Rights Reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>

#include <Library/DebugLib.h>
#include <Library/RngLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MailBoxLib.h>

STATIC_ASSERT (OFFSET_OF (MBOX_TRNG_RESPONSE, ErrCode) == 0, "Mailbox error offset");
STATIC_ASSERT (OFFSET_OF (MBOX_TRNG_RESPONSE, TrngLen) == 4, "Mailbox length offset");
STATIC_ASSERT (OFFSET_OF (MBOX_TRNG_RESPONSE, Trng) == 8, "Mailbox entropy offset");
STATIC_ASSERT (sizeof (MBOX_TRNG_RESPONSE) == 72, "Mailbox response size");

/**
  Retrieve exactly the entropy needed by a BOOLEAN RngLib entry point.

  The public entry points request at most 16 bytes, so each uses one mailbox
  request (the mailbox permits up to MAX_TRNG_SIZE bytes). A bounded response
  may contain more than requested; publish only the requested bytes after the
  entire response has been validated.

  @param[out] Buffer  Destination for the random bytes.
  @param[in]  Size    Number of bytes to retrieve, at most MAX_TRNG_SIZE.

  @retval TRUE   The mailbox returned at least Size random bytes, at most 64.
  @retval FALSE  The mailbox failed or returned an invalid response.
**/
STATIC
BOOLEAN
GetRandomBytes (
  OUT VOID  *Buffer,
  IN UINTN  Size
  )
{
  MBOX_TRNG_PARAMETER  Params;
  MBOX_TRNG_RESPONSE   Response;
  EFI_STATUS          Status;
  BOOLEAN             Success;
  volatile UINT8      *Erase;

  ASSERT ((Size != 0) && (Size <= MAX_TRNG_SIZE));
  ZeroMem (&Response, sizeof (Response));
  //
  // The audited MailBoxLib returns success without touching Response when
  // the response command is not MBOX_GET_TRNG (0x82000010). A nonzero error
  // sentinel rejects that path without reserving any valid entropy pattern.
  // The binary does not expose the outer mailbox frame's received size.
  //
  Response.ErrCode = MAX_UINT32;
  Params.TrngLen  = (UINT32)Size;
  Status         = MboxGetTrng (&Params, &Response);
  Success        = !EFI_ERROR (Status) && (Response.ErrCode == 0) &&
                   (Response.TrngLen >= Size) &&
                   (Response.TrngLen <= MAX_TRNG_SIZE);
  if (Success) {
    CopyMem (Buffer, Response.Trng, Size);
  }

  // Volatile writes keep both owned scratch erasures under optimization.
  Erase = (volatile UINT8 *)&Response;
  while (Erase < (volatile UINT8 *)(&Response + 1)) {
    *Erase++ = 0;
  }

  Erase = (volatile UINT8 *)&Params;
  while (Erase < (volatile UINT8 *)(&Params + 1)) {
    *Erase++ = 0;
  }

  return Success;
}

/**
  Generates a 16-bit random number.

  if Rand is NULL, then ASSERT().

  @param[out] Rand     Buffer pointer to store the 16-bit random value.

  @retval TRUE         Random number generated successfully.
  @retval FALSE        Failed to generate the random number.

**/
BOOLEAN
EFIAPI
GetRandomNumber16 (
  OUT UINT16  *Rand
  )
{
  ASSERT (Rand != NULL);
  if (Rand == NULL) {
    return FALSE;
  }

  return GetRandomBytes (Rand, sizeof (UINT16));
}

/**
  Generates a 32-bit random number.

  if Rand is NULL, then ASSERT().

  @param[out] Rand     Buffer pointer to store the 32-bit random value.

  @retval TRUE         Random number generated successfully.
  @retval FALSE        Failed to generate the random number.

**/
BOOLEAN
EFIAPI
GetRandomNumber32 (
  OUT UINT32  *Rand
  )
{
  ASSERT (Rand != NULL);
  if (Rand == NULL) {
    return FALSE;
  }

  return GetRandomBytes (Rand, sizeof (UINT32));
}

/**
  Generates a 64-bit random number.

  if Rand is NULL, then ASSERT().

  @param[out] Rand     Buffer pointer to store the 64-bit random value.

  @retval TRUE         Random number generated successfully.
  @retval FALSE        Failed to generate the random number.

**/
BOOLEAN
EFIAPI
GetRandomNumber64 (
  OUT UINT64  *Rand
  )
{
  ASSERT (Rand != NULL);
  if (Rand == NULL) {
    return FALSE;
  }

  return GetRandomBytes (Rand, sizeof (UINT64));
}

/**
  Generates a 128-bit random number.

  if Rand is NULL, then ASSERT().

  @param[out] Rand     Buffer pointer to store the 128-bit random value.

  @retval TRUE         Random number generated successfully.
  @retval FALSE        Failed to generate the random number.

**/
BOOLEAN
EFIAPI
GetRandomNumber128 (
  OUT UINT64  *Rand
  )
{
  ASSERT (Rand != NULL);
  if (Rand == NULL) {
    return FALSE;
  }

  return GetRandomBytes (Rand, 2 * sizeof (UINT64));
}
