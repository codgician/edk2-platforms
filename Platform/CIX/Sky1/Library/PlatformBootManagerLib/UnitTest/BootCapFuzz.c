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

UINT16 EFIAPI SwapBytes16 (UINT16 Value) { return __builtin_bswap16 (Value); }
UINT32 EFIAPI SwapBytes32 (UINT32 Value) { return __builtin_bswap32 (Value); }
UINT16 EFIAPI ReadUnaligned16 (CONST UINT16 *Buffer) {
  UINT16 Value;
  CopyMem (&Value, Buffer, sizeof (Value));
  return Value;
}

EFI_STATUS EFIAPI
Tpm2GetCapability (TPM_CAP Capability, UINT32 Property, UINT32 Count,
                   TPMI_YES_NO *MoreData, TPMS_CAPABILITY_DATA *Data)
{
  UINT8 *Buffer;
  UINTN Index;
  assert (Capability == TPM_CAP_PCRS && Property == 0 && Count == 1);
  ZeroMem (Data, sizeof (*Data));
  *MoreData = NO;
  Data->capability = SwapBytes32 (TPM_CAP_PCRS);
  Data->data.assignedPCR.count = SwapBytes32 (mPcrs.count);
  Buffer = (UINT8 *)Data->data.assignedPCR.pcrSelections;
  for (Index = 0; Index < MIN (mPcrs.count, ARRAY_SIZE (mPcrs.pcrSelections)); Index++) {
    UINT16 Hash = SwapBytes16 (mPcrs.pcrSelections[Index].hash);
    CopyMem (Buffer, &Hash, sizeof (Hash));
    Buffer[2] = mPcrs.pcrSelections[Index].sizeofSelect;
    CopyMem (Buffer + 3, mPcrs.pcrSelections[Index].pcrSelect,
             MIN (mPcrs.pcrSelections[Index].sizeofSelect, PCR_SELECT_MAX));
    Buffer += 3 + MIN (mPcrs.pcrSelections[Index].sizeofSelect, PCR_SELECT_MAX);
  }
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
Tpm2PcrExtend (TPMI_DH_PCR Pcr, TPML_DIGEST_VALUES *Digests)
{
  UINTN Index;
  UINTN Bank;
  assert (mValid);
  assert (Pcr == mExtends++);
  assert (Digests->count == mExpectedBanks);
  Bank = 0;
  for (Index = 0; Index < mPcrs.count; Index++) {
    if (!IsZeroBuffer (mPcrs.pcrSelections[Index].pcrSelect, mPcrs.pcrSelections[Index].sizeofSelect)) {
      assert (Digests->digests[Bank++].hashAlg == mPcrs.pcrSelections[Index].hash);
    }
  }
  return (Pcr == mFailPcr) ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

UINT32 EFIAPI ReadUnaligned32 (CONST UINT32 *Buffer) {
  CONST volatile UINT8 *B = (CONST volatile UINT8 *)Buffer;
  return (UINT32)B[0] | ((UINT32)B[1] << 8) | ((UINT32)B[2] << 16) | ((UINT32)B[3] << 24);
}
UINT16 EFIAPI WriteUnaligned16 (UINT16 *Buffer, UINT16 Value) {
  volatile UINT8 *B = (volatile UINT8 *)Buffer;
  B[0] = (UINT8)Value; B[1] = (UINT8)(Value >> 8);
  return Value;
}
UINT32 EFIAPI WriteUnaligned32 (UINT32 *Buffer, UINT32 Value) {
  volatile UINT8 *B = (volatile UINT8 *)Buffer;
  B[0] = (UINT8)Value; B[1] = (UINT8)(Value >> 8);
  B[2] = (UINT8)(Value >> 16); B[3] = (UINT8)(Value >> 24);
  return Value;
}

EFI_STATUS EFIAPI
Tpm2SubmitCommand (UINT32 Size, UINT8 *Input, UINT32 *OutputSize, UINT8 *Output)
{
  TPML_DIGEST_VALUES Digests;
  UINTN Index;
  UINTN Match;
  UINTN Offset = 31;
  EFI_STATUS Status;
  ZeroMem (&Digests, sizeof (Digests));
  assert (SwapBytes16 (ReadUnaligned16 ((UINT16 *)Input)) == TPM_ST_SESSIONS);
  assert (SwapBytes32 (ReadUnaligned32 ((UINT32 *)(Input + 2))) == Size);
  assert (SwapBytes32 (ReadUnaligned32 ((UINT32 *)(Input + 6))) == TPM_CC_PCR_Extend);
  Digests.count = SwapBytes32 (ReadUnaligned32 ((UINT32 *)(Input + 27)));
  assert (Digests.count <= HASH_COUNT);
  for (Index = 0; Index < Digests.count; Index++) {
    assert (Offset + 2 <= Size);
    Digests.digests[Index].hashAlg = SwapBytes16 (ReadUnaligned16 ((UINT16 *)(Input + Offset)));
    Offset += 2;
    for (Match = 0; Match < ARRAY_SIZE (mTpmFailureDigests); Match++) {
      if (mTpmFailureDigests[Match].Algorithm == Digests.digests[Index].hashAlg) {
        break;
      }
    }
    assert (Match < ARRAY_SIZE (mTpmFailureDigests));
    assert (Offset + mTpmFailureDigests[Match].Size <= Size);
    assert (CompareMem (Input + Offset, mTpmFailureDigests[Match].Digest, mTpmFailureDigests[Match].Size) == 0);
    Offset += mTpmFailureDigests[Match].Size;
  }
  assert (Offset == Size);
  Status = Tpm2PcrExtend (SwapBytes32 (ReadUnaligned32 ((UINT32 *)(Input + 10))), &Digests);
  assert (*OutputSize >= 19);
  ZeroMem (Output, 19);
  Output[0] = 0x80; Output[1] = 0x02; Output[5] = 19;
  *OutputSize = 19;
  return Status;
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
    // Compact valid seeds are reachable without synthesizing wide host ints.
    mPcrs.count = Data[1] % (ARRAY_SIZE (mPcrs.pcrSelections) + 2);
    for (Index = 0; Index < MIN (mPcrs.count, ARRAY_SIZE (mPcrs.pcrSelections)); Index++) {
      if (Size < 2 + (Index + 1) * 3) {
        break;
      }
      mPcrs.pcrSelections[Index].hash = Algorithms[Data[2 + Index * 3] % ARRAY_SIZE (Algorithms)];
      if (Data[2 + Index * 3] > 127) {
        mPcrs.pcrSelections[Index].hash = 0xffff;
      }
      mPcrs.pcrSelections[Index].sizeofSelect = Data[3 + Index * 3] % 6;
      mPcrs.pcrSelections[Index].pcrSelect[0] = Data[4 + Index * 3];
    }
  }

  mValid = (mPcrs.count != 0) && (mPcrs.count <= ARRAY_SIZE (mPcrs.pcrSelections));
  for (Index = 0; mValid && (Index < mPcrs.count); Index++) {
    TPMS_PCR_SELECTION *Selection = &mPcrs.pcrSelections[Index];
    if ((Selection->sizeofSelect == 0) || (Selection->sizeofSelect > sizeof (Selection->pcrSelect))) {
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
