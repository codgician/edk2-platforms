/** @file
  Stateful allocator/memref model with byte-exact ownership and scrub oracles.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include "TestSupport.h"

int LLVMFuzzerTestOneInput (const UINT8 *Data, size_t Size) {
  VOID *Pointers[16] = { 0 };
  UINTN Sizes[16] = { 0 };
  UINT32 Owners[16] = { 0 };
  TestInit ();
  for (size_t Pos = 0; Pos + 5 <= Size && Pos < 1024; Pos += 5) {
    UINTN Slot = Data[Pos + 1] % 16;
    UINTN Length = ((UINTN)Data[Pos + 2] << 8) | Data[Pos + 3];
    UINT32 Owner = OPTEE_SHM_CLIENT + Data[Pos + 4] % 5;
    UINT8 *P = Pointers[Slot];
    switch (Data[Pos] % 5) {
      case 0:
        if (P != NULL) { break; }
        if (Data[Pos + 4] == 255) { Length = MAX_UINTN; }
        UINTN Alignment = (UINTN)1 << (Data[Pos + 4] % 24);
        P = OpteeShmAllocOwned (Length, Alignment, Owner);
        if (P != NULL) {
          assert (((UINTN)P & (Alignment - 1)) == 0);
          assert (OpteeShmIsOwned ((UINTN)P, Length, Owner));
          for (UINTN I = 0; I < MAX (1, (Length + 4095) / 4096) * 4096; I++) { assert (P[I] == 0); }
          for (UINTN I = 0; I < 16; I++) {
            if (Pointers[I] == NULL) { continue; }
            UINTN A = (UINTN)Pointers[I], B = (UINTN)P;
            assert (B + MAX (Length, 1) <= A || A + MAX (Sizes[I], 1) <= B);
          }
          Pointers[Slot] = P; Sizes[Slot] = Length; Owners[Slot] = Owner;
          memset (P, (int)Slot + 1, Length);
        }
        break;
      case 1:
        if (P != NULL) {
          assert (!OpteeShmFreeOwned (P + 1, Owners[Slot]));
          assert (!OpteeShmFreeOwned (P, Owners[Slot] == 5 ? 1 : Owners[Slot] + 1));
          for (UINTN I = 0; I < Sizes[Slot]; I++) { assert (P[I] == Slot + 1); }
          assert (OpteeShmFreeOwned (P, Owners[Slot]));
          for (UINTN I = 0; I < MAX (1, (Sizes[Slot] + 4095) / 4096) * 4096; I++) { assert (P[I] == 0); }
          assert (!OpteeShmContains ((UINTN)P, 1));
          assert (!OpteeShmFreeOwned (P, Owners[Slot]));
          Pointers[Slot] = NULL;
        }
        break;
      case 2:
        if (P != NULL) {
          OPTEE_MSG_PARAM Param = { 0 };
          VOID *Out; UINTN OutSize;
          UINTN Offset = Data[Pos + 2];
          Param.Attr = OPTEE_MSG_ATTR_TYPE_TMEM_INPUT;
          Param.U.Tmem.BufPtr = (UINTN)P + Offset;
          Param.U.Tmem.ShmRef = (UINTN)P;
          Param.U.Tmem.Size = Length;
          BOOLEAN Expected = Owners[Slot] >= OPTEE_SHM_RPC_APPL && Offset <= Sizes[Slot] && Length <= Sizes[Slot] - Offset;
          assert (OpteeMsgParamToBuffer (&Param, &Out, &OutSize) == Expected);
          if (Expected) { assert (Out == P + Offset && OutSize == Length); }
          Param.Attr = OPTEE_MSG_ATTR_TYPE_RMEM_INPUT;
          Param.U.Rmem.Offs = MAX_UINT64;
          assert (!OpteeMsgParamToBuffer (&Param, &Out, &OutSize));
        }
        break;
      case 3:
        assert (!OpteeShmFreeOwned ((VOID *)(UINTN)Length, Owner));
        break;
      case 4:
        if (P != NULL) {
          assert (OpteeShmContains ((UINTN)P, Sizes[Slot]));
          assert (!OpteeShmContains ((UINTN)P, Sizes[Slot] + 1));
        }
        break;
    }
  }
  TestFini ();
  return 0;
}
