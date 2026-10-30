/** @file
  PCR banks measurable by the Sky1 Tcg2Dxe build.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef SKY1_PP_HASH_H_
#define SKY1_PP_HASH_H_

#include <Protocol/Tcg2Protocol.h>
#include <Library/PcdLib.h>

/** Return the linked hash engines allowed by both measurement PCDs.

  Sky1Common.dsc.inc links SHA1, SHA256 and SHA384 into Tcg2Dxe. The ceiling
  remains necessary if the router bitmap has not yet been initialized, or if
  another module registers an engine that Tcg2Dxe cannot use. Keep this ceiling
  synchronized with that component's hash-instance library bindings.

  @return  Firmware-measurable PCR bank bitmap (possibly empty).
**/
STATIC
UINT32
PpHashableBanks (
  VOID
  )
{
  return (EFI_TCG2_BOOT_HASH_ALG_SHA1 | EFI_TCG2_BOOT_HASH_ALG_SHA256 |
          EFI_TCG2_BOOT_HASH_ALG_SHA384) &
         PcdGet32 (PcdTcg2HashAlgorithmBitmap) & PcdGet32 (PcdTpm2HashMask);
}

#endif
