/** @file Checked static-PCR recovery shared by the gate and resident fTPM.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#ifndef SKY1_TPM_BOOT_CAP_H_
#define SKY1_TPM_BOOT_CAP_H_

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/Tpm2CommandLib.h>
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
  TPML_PCR_SELECTION  Pcrs;
  TPML_DIGEST_VALUES  Digests;
  TPMS_PCR_SELECTION  *Selection;
  EFI_STATUS          Status;
  UINTN               Index;
  UINTN               Digest;
  UINT32              Seen;
  UINT32              Pcr;

  // Tpm2GetCapability bounds the bank count and every selection width.
  Status = Tpm2GetCapabilityPcrs (&Pcrs);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ZeroMem (&Digests, sizeof (Digests));
  Seen = 0;
  for (Index = 0; Index < Pcrs.count; Index++) {
    Selection = &Pcrs.pcrSelections[Index];
    if (Selection->sizeofSelect == 0) {
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

    Seen                                  |= 1U << Digest;
    Digests.digests[Digests.count].hashAlg = Selection->hash;
    CopyMem (&Digests.digests[Digests.count].digest, mTpmFailureDigests[Digest].Digest, sizeof (mTpmFailureDigests[Digest].Digest));
    Digests.count++;
  }

  if (Digests.count == 0) {
    return EFI_COMPROMISED_DATA;
  }

  for (Pcr = 0; Pcr < 8; Pcr++) {
    Status = Tpm2PcrExtend (Pcr, &Digests);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  return EFI_SUCCESS;
}

#endif
