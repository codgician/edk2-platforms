/** @file Stateful REE FS model oracle; production allocator and parser are used. */
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "TestSupport.h"
#include "../OpteeClientInternal.h"

typedef struct { int Live; size_t Size; UINT8 Data[128]; } MODEL_FILE;
typedef struct { UINT64 Id; int File; int Live; } MODEL_FD;
static union { UINT64 Align; UINT8 Bytes[OPTEE_MSG_ARG_SIZE (3)]; } mRequest;
static OPTEE_MSG_ARG *Arg;
static UINT8 *Names, *Data;
static void Start (UINT64 Op, UINT32 Count, UINT64 Fd, UINT64 Offset) {
  memset (&mRequest, 0, sizeof (mRequest));
  Arg = (OPTEE_MSG_ARG *)mRequest.Bytes;
  Arg->NumParams = Count;
  Arg->Params[0].Attr = OPTEE_MSG_ATTR_TYPE_VALUE_INPUT;
  Arg->Params[0].U.Value.A = Op;
  Arg->Params[0].U.Value.B = Fd;
  Arg->Params[0].U.Value.C = Offset;
}
static void Mem (int Index, void *Buffer, UINTN Size, UINT64 Attr, void *Base) {
  Arg->Params[Index].Attr = Attr;
  Arg->Params[Index].U.Tmem.BufPtr = (UINTN)Buffer;
  Arg->Params[Index].U.Tmem.ShmRef = (UINTN)Base;
  Arg->Params[Index].U.Tmem.Size = Size;
}
static void Path (int Param, int File) {
  Names[(Param - 1) * 32] = (UINT8)('a' + File);
  Names[(Param - 1) * 32 + 1] = 0;
  Mem (Param, Names + (Param - 1) * 32, 32, OPTEE_MSG_ATTR_TYPE_TMEM_INPUT, Names);
}
int LLVMFuzzerTestOneInput (const uint8_t *Input, size_t InputSize) {
  MODEL_FILE Files[4] = {0};
  MODEL_FD Fds[64] = {0};
  UINT64 LastId = 0;
  size_t Step;
  TestInit ();
  Names = OpteeShmAllocOwned (64, 8, OPTEE_SHM_RPC_APPL);
  Data = OpteeShmAllocOwned (128, 8, OPTEE_SHM_RPC_APPL);
  assert (Names && Data);
  for (Step = 0; Step + 4 <= InputSize && Step < 4096; Step += 4) {
    unsigned Op = Input[Step] % 14, File = Input[Step + 1] % 4;
    unsigned Slot = Input[Step + 1] % 64, Offset = Input[Step + 2] % 129;
    unsigned Length = Input[Step + 3] % 129;
    MODEL_FD *Fd = &Fds[Slot];
    MODEL_FILE *F = Fd->Live ? &Files[Fd->File] : NULL;
    TEEC_Result Result;
    if (Op < 2) {
      unsigned Free;
      for (Free = 0; Free < 64 && Fds[Free].Live; Free++) {}
      Start (Op ? OPTEE_RPC_FS_CREATE : OPTEE_RPC_FS_OPEN, 3, 0, 0);
      Path (1, File);
      Arg->Params[2].Attr = OPTEE_MSG_ATTR_TYPE_VALUE_OUTPUT;
      Result = OpteeReeFsHandle (Arg);
      if ((!Op && !Files[File].Live)) {
        assert (Result == TEEC_ERROR_ITEM_NOT_FOUND);
      } else if (Free == 64) {
        assert (Result == TEEC_ERROR_OUT_OF_MEMORY);
      } else {
        assert (Result == TEEC_SUCCESS);
        assert (Arg->Params[2].U.Value.A > LastId && Arg->Params[2].U.Value.A <= INT32_MAX);
        LastId = Arg->Params[2].U.Value.A;
        Fds[Free] = (MODEL_FD){ LastId, (int)File, 1 };
        if (Op) { memset (&Files[File], 0, sizeof (Files[File])); Files[File].Live = 1; }
      }
    } else if (Op == 2) {
      Start (OPTEE_RPC_FS_CLOSE, 1, Fd->Id, 0);
      Result = OpteeReeFsHandle (Arg);
      assert ((Result == TEEC_SUCCESS) == !!Fd->Live);
      Fd->Live = 0;
    } else if (Op == 3 || Op == 4) {
      int Write = Op == 4;
      if (Length > 128 - (Offset < 128 ? Offset : 128)) Length = 128 - (Offset < 128 ? Offset : 128);
      memset (Data, Input[Step + 3], 128);
      Start (Write ? OPTEE_RPC_FS_WRITE : OPTEE_RPC_FS_READ, 2, Fd->Id, Offset);
      Mem (1, Data, Length, Write ? OPTEE_MSG_ATTR_TYPE_TMEM_INPUT : OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT, Data);
      Result = OpteeReeFsHandle (Arg);
      if (!Fd->Live) { assert (Result != TEEC_SUCCESS); continue; }
      assert (Result == TEEC_SUCCESS);
      if (Write && Length) {
        if (Offset > F->Size) memset (F->Data + F->Size, 0, Offset - F->Size);
        memset (F->Data + Offset, Input[Step + 3], Length);
        if (Offset + Length > F->Size) F->Size = Offset + Length;
      } else if (!Write) {
        size_t Copy = Offset >= F->Size ? 0 : F->Size - Offset;
        if (Copy > Length) Copy = Length;
        assert (Arg->Params[1].U.Tmem.Size == Copy);
        if (Copy) assert (memcmp (Data, F->Data + Offset, Copy) == 0);
      }
    } else if (Op == 5) {
      Start (OPTEE_RPC_FS_TRUNCATE, 1, Fd->Id, Offset);
      Result = OpteeReeFsHandle (Arg);
      assert ((Result == TEEC_SUCCESS) == !!Fd->Live);
      if (Fd->Live) {
        if (Offset > F->Size) memset (F->Data + F->Size, 0, Offset - F->Size);
        else memset (F->Data + Offset, 0, F->Size - Offset);
        F->Size = Offset;
      }
    } else if (Op == 6) {
      Start (OPTEE_RPC_FS_REMOVE, 2, 0, 0); Path (1, File);
      Result = OpteeReeFsHandle (Arg);
      assert ((Result == TEEC_SUCCESS) == !!Files[File].Live);
      if (Files[File].Live) {
        memset (&Files[File], 0, sizeof (Files[File]));
        for (unsigned I = 0; I < 64; I++) if (Fds[I].File == (int)File) Fds[I].Live = 0;
      }
    } else if (Op == 7) {
      unsigned New = Input[Step + 2] % 4;
      int Overwrite = Input[Step + 3] & 1;
      Start (OPTEE_RPC_FS_RENAME, 3, Overwrite, 0); Path (1, File); Path (2, New);
      Result = OpteeReeFsHandle (Arg);
      if (!Files[File].Live) assert (Result == TEEC_ERROR_ITEM_NOT_FOUND);
      else if (New != File && Files[New].Live && !Overwrite) assert (Result == TEEC_ERROR_ACCESS_CONFLICT);
      else {
        assert (Result == TEEC_SUCCESS);
        if (New != File) {
          Files[New] = Files[File]; memset (&Files[File], 0, sizeof (Files[File]));
          for (unsigned I = 0; I < 64; I++) {
            if (Fds[I].File == (int)New) Fds[I].Live = 0;
            else if (Fds[I].File == (int)File) Fds[I].File = (int)New;
          }
        }
      }
    } else if (Op == 8) {
      Start (OPTEE_RPC_FS_TRUNCATE, 1, Fd->Id, UINT64_MAX);
      assert (OpteeReeFsHandle (Arg) != TEEC_SUCCESS);
    } else if (Op == 9) {
      Start (OPTEE_RPC_FS_WRITE, 2, Fd->Id, 0);
      Mem (1, Data, Length, OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT, Data);
      assert (OpteeReeFsHandle (Arg) == TEEC_ERROR_BAD_PARAMETERS);
    } else if (Op == 10) {
      TestFailAllocation (1);
      Start (OPTEE_RPC_FS_TRUNCATE, 1, Fd->Id, 8192);
      Result = OpteeReeFsHandle (Arg);
      assert (Result == (Fd->Live ? TEEC_ERROR_OUT_OF_MEMORY : TEEC_ERROR_GENERIC));
      TestFailAllocation (0);
    } else if (Op == 11) {
      Start (OPTEE_RPC_FS_READ, 2, Fd->Id, UINT64_MAX);
      Mem (1, Data, Length, OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT, Data);
      Result = OpteeReeFsHandle (Arg);
      assert ((Result == TEEC_SUCCESS) == !!Fd->Live);
      if (Fd->Live) assert (Arg->Params[1].U.Tmem.Size == 0);
    } else if (Op == 12) {
      Start (OPTEE_RPC_FS_CREATE, 3, 0, 0);
      memset (Names, 'x', 64);
      Mem (1, Names, 64, OPTEE_MSG_ATTR_TYPE_TMEM_INPUT, Names);
      Arg->Params[2].Attr = OPTEE_MSG_ATTR_TYPE_VALUE_OUTPUT;
      assert (OpteeReeFsHandle (Arg) == TEEC_ERROR_BAD_PARAMETERS);
    } else {
      Start (OPTEE_RPC_FS_TRUNCATE, 1, Fd->Id, 0);
      Arg->Params[0].Attr |= OPTEE_MSG_ATTR_META;
      assert (OpteeReeFsHandle (Arg) == TEEC_ERROR_BAD_PARAMETERS);
    }
    /* Observe every reachable model file after each operation, including rejects. */
    for (unsigned I = 0; I < 64; I++) if (Fds[I].Live) {
      MODEL_FILE *Expected = &Files[Fds[I].File];
      Start (OPTEE_RPC_FS_READ, 2, Fds[I].Id, 0);
      Mem (1, Data, 128, OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT, Data);
      assert (OpteeReeFsHandle (Arg) == TEEC_SUCCESS);
      assert (Arg->Params[1].U.Tmem.Size == Expected->Size);
      assert (memcmp (Data, Expected->Data, Expected->Size) == 0);
    }
  }
  TestFini ();
  return 0;
}
