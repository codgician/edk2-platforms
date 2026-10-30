/** @file Checked static-PCR recovery shared by the gate and resident fTPM.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#ifndef SKY1_TPM_BOOT_CAP_H_
#define SKY1_TPM_BOOT_CAP_H_

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/Tpm2CommandLib.h>
#include <Library/Tpm2DeviceLib.h>
#include "TpmFailureDigest.h"

/**
  Cap PCRs 0-7 directly in every allocated bank, without the Tcg2 protocol.

  @retval EFI_SUCCESS           Every bank/PCR extension was confirmed.
  @retval EFI_COMPROMISED_DATA  Allocation cannot prove complete coverage.
  @return TPM transport error if discovery or an extension fails.
**/
STATIC
EFI_STATUS
PlatformTpmCapPcrs (
  VOID
  )
{
  TPML_PCR_SELECTION   Pcrs;
  TPMS_CAPABILITY_DATA Capability;
  TPMI_YES_NO          MoreData;
  UINT8                *Buffer;
  UINTN                Remaining;
  UINT32               DigestCount;
  TPMS_PCR_SELECTION   *Selection;
  EFI_STATUS           Status;
  UINTN                Index;
  UINTN                Digest;
  UINT32               Seen;
  UINT32               Pcr;
  UINT8                Command[31 + HASH_COUNT * (sizeof (UINT16) + SHA512_DIGEST_SIZE)];
  UINT8                Response[19];
  UINTN                CommandSize;
  UINT32               ResponseSize;

  ZeroMem (&Capability, sizeof (Capability));
  Status = Tpm2GetCapability (TPM_CAP_PCRS, 0, 1, &MoreData, &Capability);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ZeroMem (&Pcrs, sizeof (Pcrs));
  Pcrs.count = SwapBytes32 (Capability.data.assignedPCR.count);
  if ((MoreData != NO) || (SwapBytes32 (Capability.capability) != TPM_CAP_PCRS) ||
      (Pcrs.count == 0) || (Pcrs.count > ARRAY_SIZE (Pcrs.pcrSelections)))
  {
    return EFI_COMPROMISED_DATA;
  }

  Buffer = (UINT8 *)Capability.data.assignedPCR.pcrSelections;
  Remaining = sizeof (Capability.data.assignedPCR.pcrSelections);
  for (Index = 0; Index < Pcrs.count; Index++) {
    Selection = &Pcrs.pcrSelections[Index];
    if (Remaining < sizeof (UINT16) + sizeof (UINT8)) {
      return EFI_COMPROMISED_DATA;
    }

    Selection->hash = SwapBytes16 (ReadUnaligned16 ((UINT16 *)Buffer));
    Selection->sizeofSelect = Buffer[sizeof (UINT16)];
    Buffer += sizeof (UINT16) + sizeof (UINT8);
    Remaining -= sizeof (UINT16) + sizeof (UINT8);
    if ((Selection->sizeofSelect > sizeof (Selection->pcrSelect)) ||
        (Selection->sizeofSelect > Remaining))
    {
      return EFI_COMPROMISED_DATA;
    }

    CopyMem (Selection->pcrSelect, Buffer, Selection->sizeofSelect);
    Buffer += Selection->sizeofSelect;
    Remaining -= Selection->sizeofSelect;
  }

  DigestCount = 0;
  Seen = 0;
  ZeroMem (Command, sizeof (Command));
  Buffer = Command + 31;
  for (Index = 0; Index < Pcrs.count; Index++) {
    Selection = &Pcrs.pcrSelections[Index];
    if ((Selection->sizeofSelect == 0) || (Selection->sizeofSelect > sizeof (Selection->pcrSelect))) {
      return EFI_COMPROMISED_DATA;
    }

    if (IsZeroBuffer (Selection->pcrSelect, Selection->sizeofSelect)) {
      continue;
    }

    for (Digest = 0; Digest < ARRAY_SIZE (mTpmFailureDigests); Digest++) {
      if (mTpmFailureDigests[Digest].Algorithm == Selection->hash) {
        break;
      }
    }

    if ((Digest == ARRAY_SIZE (mTpmFailureDigests)) ||
        ((Seen & (1U << Digest)) != 0) || (Selection->pcrSelect[0] != 0xFF))
    {
      return EFI_COMPROMISED_DATA;
    }

    Seen |= 1U << Digest;
    WriteUnaligned16 ((UINT16 *)Buffer, SwapBytes16 (Selection->hash));
    Buffer += sizeof (UINT16);
    CopyMem (Buffer, mTpmFailureDigests[Digest].Digest, mTpmFailureDigests[Digest].Size);
    Buffer += mTpmFailureDigests[Digest].Size;
    DigestCount++;
  }

  if (DigestCount == 0) {
    return EFI_COMPROMISED_DATA;
  }

  // Use a password session with empty auth for static-PCR extends. Marshal
  // once and check the entire reply, not just its untrusted success header.
  CommandSize = (UINTN)(Buffer - Command);
  WriteUnaligned16 ((UINT16 *)Command, SwapBytes16 (TPM_ST_SESSIONS));
  WriteUnaligned32 ((UINT32 *)(Command + 2), SwapBytes32 ((UINT32)CommandSize));
  WriteUnaligned32 ((UINT32 *)(Command + 6), SwapBytes32 (TPM_CC_PCR_Extend));
  WriteUnaligned32 ((UINT32 *)(Command + 14), SwapBytes32 (9));
  WriteUnaligned32 ((UINT32 *)(Command + 18), SwapBytes32 (TPM_RS_PW));
  WriteUnaligned32 ((UINT32 *)(Command + 27), SwapBytes32 (DigestCount));
  for (Pcr = 0; Pcr < 8; Pcr++) {
    WriteUnaligned32 ((UINT32 *)(Command + 10), SwapBytes32 (Pcr));
    ZeroMem (Response, sizeof (Response));
    ResponseSize = sizeof (Response);
    Status = Tpm2SubmitCommand ((UINT32)CommandSize, Command, &ResponseSize, Response);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if ((ResponseSize != sizeof (Response)) ||
        (SwapBytes16 (ReadUnaligned16 ((UINT16 *)Response)) != TPM_ST_SESSIONS) ||
        (SwapBytes32 (ReadUnaligned32 ((UINT32 *)(Response + 2))) != sizeof (Response)) ||
        (SwapBytes32 (ReadUnaligned32 ((UINT32 *)(Response + 6))) != TPM_RC_SUCCESS) ||
        !IsZeroBuffer (Response + 10, 6) || (Response[16] > 1) ||
        !IsZeroBuffer (Response + 17, 2))
    {
      return EFI_DEVICE_ERROR;
    }
  }

  return EFI_SUCCESS;
}

#endif
