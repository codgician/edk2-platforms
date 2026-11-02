/** @file Exercise the production all-bank cap against hostile allocations.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <Library/BaseMemoryLib.h>
#include "../TpmBootCap.h"

STATIC TPML_PCR_SELECTION  mPcrs;
STATIC UINT32             mExtends;
STATIC UINT32             mExpectedBanks;
STATIC BOOLEAN            mValid;
STATIC UINT8              mFailPcr;

BOOLEAN EFIAPI
IsZeroBuffer (CONST VOID *Buffer, UINTN Size)
{
  CONST UINT8 *Bytes = Buffer;
  while (Size-- != 0) {
    if (*Bytes++ != 0) {
      return FALSE;
    }
  }
  return TRUE;
}

EFI_STATUS EFIAPI
Tpm2GetCapabilityPcrs (TPML_PCR_SELECTION *Pcrs)
{
  CopyMem (Pcrs, &mPcrs, sizeof (mPcrs));
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
Tpm2PcrExtend (TPMI_DH_PCR Pcr, TPML_DIGEST_VALUES *Digests)
{
  UINTN Index;
  UINTN Bank;
  UINTN Match;
  assert (mValid);
  assert (Pcr == mExtends++);
  assert (Digests->count == mExpectedBanks);
  Bank = 0;
  for (Index = 0; Index < mPcrs.count; Index++) {
    if (!IsZeroBuffer (mPcrs.pcrSelections[Index].pcrSelect, mPcrs.pcrSelections[Index].sizeofSelect)) {
      assert (Digests->digests[Bank].hashAlg == mPcrs.pcrSelections[Index].hash);
      for (Match = 0; mTpmFailureDigests[Match].Algorithm != Digests->digests[Bank].hashAlg; Match++) {
      }
      assert (CompareMem (&Digests->digests[Bank++].digest, mTpmFailureDigests[Match].Digest,
                          sizeof (mTpmFailureDigests[Match].Digest)) == 0);
    }
  }
  return (Pcr == mFailPcr) ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

int LLVMFuzzerTestOneInput (const uint8_t *Data, size_t Size)
{
  STATIC CONST TPMI_ALG_HASH Algorithms[] = {
    TPM_ALG_SHA1, TPM_ALG_SHA256, TPM_ALG_SHA384, TPM_ALG_SHA512, TPM_ALG_SM3_256
  };
  UINTN Index;
  UINTN Other;
  UINTN Alg;
  EFI_STATUS Status;

  ZeroMem (&mPcrs, sizeof (mPcrs));
  mExtends = mExpectedBanks = 0;
  mFailPcr = (Size == 0) ? 255 : Data[0];
  if (Size > 1) {
    // Tpm2GetCapability bounds the count and widths this library returns.
    mPcrs.count = Data[1] % (ARRAY_SIZE (mPcrs.pcrSelections) + 1);
    for (Index = 0; Index < mPcrs.count; Index++) {
      if (Size < 2 + (Index + 1) * 3) {
        break;
      }
      mPcrs.pcrSelections[Index].hash = Algorithms[Data[2 + Index * 3] % ARRAY_SIZE (Algorithms)];
      if (Data[2 + Index * 3] > 127) {
        mPcrs.pcrSelections[Index].hash = 0xffff;
      }
      mPcrs.pcrSelections[Index].sizeofSelect = Data[3 + Index * 3] % (PCR_SELECT_MAX + 1);
      mPcrs.pcrSelections[Index].pcrSelect[0] = Data[4 + Index * 3];
    }
  }

  mValid = (mPcrs.count != 0);
  for (Index = 0; mValid && (Index < mPcrs.count); Index++) {
    TPMS_PCR_SELECTION *Selection = &mPcrs.pcrSelections[Index];
    if (Selection->sizeofSelect == 0) {
      mValid = FALSE;
    } else if (Selection->pcrSelect[0] != 0) {
      if (Selection->pcrSelect[0] != 255) {
        mValid = FALSE;
      }
      for (Alg = 0; Alg < ARRAY_SIZE (Algorithms); Alg++) {
        if (Algorithms[Alg] == Selection->hash) {
          break;
        }
      }
      if (Alg == ARRAY_SIZE (Algorithms)) {
        mValid = FALSE;
      }
      for (Other = 0; Other < Index; Other++) {
        if ((mPcrs.pcrSelections[Other].hash == Selection->hash) &&
            (mPcrs.pcrSelections[Other].pcrSelect[0] != 0)) {
          mValid = FALSE;
        }
      }
      mExpectedBanks++;
    }
  }
  mValid = mValid && (mExpectedBanks != 0);
  Status = PlatformTpmCapPcrs ();
  if (!mValid) {
    assert (Status == EFI_COMPROMISED_DATA && mExtends == 0);
  } else if (mFailPcr < 8) {
    assert (Status == EFI_DEVICE_ERROR && mExtends == mFailPcr + 1);
  } else {
    assert (Status == EFI_SUCCESS && mExtends == 8);
  }
  return 0;
}
