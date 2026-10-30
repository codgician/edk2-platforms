/** @file Regression tests for the real in-memory REE filesystem. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "TestSupport.h"
#include "../OpteeClientInternal.h"

#define FILE_CAP (1024U * 1024U)
static union { UINT64 Align; UINT8 Bytes[OPTEE_MSG_ARG_SIZE (3)]; } mStorage;
static OPTEE_MSG_ARG *mArg;
static char *mName;
static UINT8 *mIo;

static TEEC_Result CheckedHandle (OPTEE_MSG_ARG *Arg) {
  UINTN Before = TestDiagnosticCount;
  TEEC_Result Result = OpteeReeFsHandle (Arg);
  assert (TestDiagnosticCount - Before == (Result != TEEC_SUCCESS ? 1 : 0));
  assert (strstr (TestDiagnostic, "SECRET-CANARY") == NULL);
  return Result;
}
#define OpteeReeFsHandle CheckedHandle

static void Begin (UINT64 Op, UINT32 Count, UINT64 Fd, UINT64 Offset) {
  memset (&mStorage, 0, sizeof (mStorage));
  mArg = (OPTEE_MSG_ARG *)mStorage.Bytes;
  mArg->NumParams = Count;
  mArg->Params[0].Attr = OPTEE_MSG_ATTR_TYPE_VALUE_INPUT;
  mArg->Params[0].U.Value.A = Op;
  mArg->Params[0].U.Value.B = Fd;
  mArg->Params[0].U.Value.C = Offset;
}
static void Mem (UINTN Index, void *Buffer, UINTN Size, UINT64 Attr) {
  mArg->Params[Index].Attr = Attr;
  mArg->Params[Index].U.Tmem.BufPtr = (UINTN)Buffer;
  mArg->Params[Index].U.Tmem.ShmRef = (Index == 1 && Buffer == mName) ? (UINTN)mName : (UINTN)mIo;
  mArg->Params[Index].U.Tmem.Size = Size;
}
static TEEC_Result Open (const char *Name, int Create, UINT64 *Fd) {
  strcpy (mName, Name);
  Begin (Create ? OPTEE_RPC_FS_CREATE : OPTEE_RPC_FS_OPEN, 3, 0, 0);
  Mem (1, mName, 64, OPTEE_MSG_ATTR_TYPE_TMEM_INPUT);
  mArg->Params[2].Attr = OPTEE_MSG_ATTR_TYPE_VALUE_OUTPUT;
  TEEC_Result Result = OpteeReeFsHandle (mArg);
  if (Result == TEEC_SUCCESS) *Fd = mArg->Params[2].U.Value.A;
  return Result;
}
static TEEC_Result Simple (UINT64 Op, UINT64 Fd, UINT64 Offset) {
  Begin (Op, 1, Fd, Offset);
  return OpteeReeFsHandle (mArg);
}
static TEEC_Result Io (UINT64 Fd, UINT64 Offset, UINTN Size, int Write) {
  Begin (Write ? OPTEE_RPC_FS_WRITE : OPTEE_RPC_FS_READ, 2, Fd, Offset);
  Mem (1, mIo, Size, Write ? OPTEE_MSG_ATTR_TYPE_TMEM_INPUT : OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT);
  return OpteeReeFsHandle (mArg);
}
static TEEC_Result Remove (const char *Name) {
  strcpy (mName, Name);
  Begin (OPTEE_RPC_FS_REMOVE, 2, 0, 0);
  Mem (1, mName, 64, OPTEE_MSG_ATTR_TYPE_TMEM_INPUT);
  return OpteeReeFsHandle (mArg);
}
static void CheckData (UINT64 Fd) {
  memset (mIo, 0, 4);
  assert (Io (Fd, 0, 4, 0) == TEEC_SUCCESS);
  assert (mArg->Params[1].U.Tmem.Size == 4);
  assert (memcmp (mIo, "data", 4) == 0);
}
static TEEC_Result Rename (const char *Old, const char *New, int Overwrite) {
  strcpy (mName, Old);
  strcpy ((char *)mIo, New);
  Begin (OPTEE_RPC_FS_RENAME, 3, Overwrite, 0);
  Mem (1, mName, 64, OPTEE_MSG_ATTR_TYPE_TMEM_INPUT);
  Mem (2, mIo, 64, OPTEE_MSG_ATTR_TYPE_TMEM_INPUT);
  return OpteeReeFsHandle (mArg);
}
static void Directions (UINT64 Fd) {
  static const UINT64 Wrong[] = {
    OPTEE_MSG_ATTR_TYPE_NONE, OPTEE_MSG_ATTR_TYPE_VALUE_INPUT,
    OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT, OPTEE_MSG_ATTR_TYPE_TMEM_INOUT,
    OPTEE_MSG_ATTR_TYPE_TMEM_INPUT | OPTEE_MSG_ATTR_META,
    OPTEE_MSG_ATTR_TYPE_RMEM_INPUT
  };
  for (unsigned I = 0; I < sizeof (Wrong) / sizeof (Wrong[0]); I++) {
    strcpy (mName, "a");
    Begin (OPTEE_RPC_FS_CREATE, 3, 0, 0);
    Mem (1, mName, 64, Wrong[I]);
    mArg->Params[2].Attr = OPTEE_MSG_ATTR_TYPE_VALUE_OUTPUT;
    assert (OpteeReeFsHandle (mArg) == TEEC_ERROR_BAD_PARAMETERS);
    TestExpectDiagnostic ("REE-FS open framing", Wrong[I], TEEC_ERROR_BAD_PARAMETERS);
    CheckData (Fd);
  }
  Begin (OPTEE_RPC_FS_CREATE, 3, 0, 0);
  Mem (1, mName, 64, OPTEE_MSG_ATTR_TYPE_TMEM_INPUT);
  mArg->Params[2].Attr = OPTEE_MSG_ATTR_TYPE_VALUE_INOUT;
  assert (OpteeReeFsHandle (mArg) == TEEC_ERROR_BAD_PARAMETERS);
  TestExpectDiagnostic ("REE-FS open framing", OPTEE_MSG_ATTR_TYPE_VALUE_INOUT, TEEC_ERROR_BAD_PARAMETERS);
  CheckData (Fd);
  Begin (OPTEE_RPC_FS_READ, 2, Fd, 0);
  Mem (1, mIo, 4, OPTEE_MSG_ATTR_TYPE_TMEM_INPUT);
  assert (OpteeReeFsHandle (mArg) == TEEC_ERROR_BAD_PARAMETERS);
  TestExpectDiagnostic ("REE-FS IO framing", OPTEE_MSG_ATTR_TYPE_TMEM_INPUT, TEEC_ERROR_BAD_PARAMETERS);
  Begin (OPTEE_RPC_FS_CLOSE, 2, Fd, 0);
  assert (OpteeReeFsHandle (mArg) == TEEC_ERROR_BAD_PARAMETERS);
  TestExpectDiagnostic ("REE-FS close framing", 2, TEEC_ERROR_BAD_PARAMETERS);
  CheckData (Fd);
  Begin (OPTEE_RPC_FS_TRUNCATE, 1, Fd, 0);
  mArg->Params[0].Attr |= OPTEE_MSG_ATTR_META;
  assert (OpteeReeFsHandle (mArg) == TEEC_ERROR_BAD_PARAMETERS);
  TestExpectDiagnostic ("REE-FS framing", OPTEE_MSG_ATTR_TYPE_VALUE_INPUT | OPTEE_MSG_ATTR_META, TEEC_ERROR_BAD_PARAMETERS);
  CheckData (Fd);
}
static void Sequences (UINT64 Fd) {
  UINT64 Handles[64], Other, Last = Fd;
  char Name[32];
  /* A growth failure cannot change the prior size, contents or handle. */
  TestFailAllocation (1);
  assert (Simple (OPTEE_RPC_FS_TRUNCATE, Fd, 8192) == TEEC_ERROR_OUT_OF_MEMORY);
  TestExpectDiagnostic ("REE-FS capacity", 8192, TEEC_ERROR_OUT_OF_MEMORY);
  TestFailAllocation (0);
  CheckData (Fd);
  TestFailAllocation (1);
  assert (Io (Fd, 8192, 4, 1) == TEEC_ERROR_OUT_OF_MEMORY);
  TestExpectDiagnostic ("REE-FS capacity", 8196, TEEC_ERROR_OUT_OF_MEMORY);
  TestFailAllocation (0);
  CheckData (Fd);
  assert (Io (Fd, UINT64_MAX, 16, 0) == TEEC_SUCCESS);
  assert (mArg->Params[1].U.Tmem.Size == 0);
  assert (Io (Fd, UINT64_MAX, 1, 1) == TEEC_ERROR_BAD_PARAMETERS);
  TestExpectDiagnostic ("REE-FS offset", UINT64_MAX, TEEC_ERROR_BAD_PARAMETERS);
  assert (Simple (OPTEE_RPC_FS_TRUNCATE, Fd, 2) == TEEC_SUCCESS);
  assert (Simple (OPTEE_RPC_FS_TRUNCATE, Fd, 8) == TEEC_SUCCESS);
  memset (mIo, 0xff, 8);
  assert (Io (Fd, 0, 8, 0) == TEEC_SUCCESS);
  assert (memcmp (mIo, "da\0\0\0\0\0\0", 8) == 0);
  mIo[0] = 0x7b;
  assert (Io (Fd, 16, 1, 1) == TEEC_SUCCESS);
  assert (Io (Fd, 8, 9, 0) == TEEC_SUCCESS);
  for (unsigned I = 0; I < 8; I++) assert (mIo[I] == 0);
  assert (mIo[8] == 0x7b);
  assert (Open ("b", 1, &Other) == TEEC_SUCCESS);
  assert (Rename ("a", "b", 0) == TEEC_ERROR_ACCESS_CONFLICT);
  TestExpectDiagnostic ("REE-FS overwrite", 0, TEEC_ERROR_ACCESS_CONFLICT);
  assert (Rename ("a", "b", 1) == TEEC_SUCCESS);
  assert (Io (Other, 0, 1, 0) != TEEC_SUCCESS);
  TestExpectDiagnostic ("REE-FS descriptor", Other, TEEC_ERROR_GENERIC);
  assert (Io (Fd, 16, 1, 0) == TEEC_SUCCESS && mIo[0] == 0x7b);
  assert (Rename ("b", "b", 0) == TEEC_SUCCESS);
  assert (Remove ("b") == TEEC_SUCCESS);
  assert (Io (Fd, 0, 1, 0) != TEEC_SUCCESS);
  TestExpectDiagnostic ("REE-FS descriptor", Fd, TEEC_ERROR_GENERIC);
  /* Names accept exactly 1..31 bytes with a NUL in the supplied memref. */
  memset (Name, 'n', 31); Name[31] = 0;
  assert (Open (Name, 1, &Other) == TEEC_SUCCESS);
  assert (Remove (Name) == TEEC_SUCCESS);
  assert (Open ("", 1, &Other) == TEEC_ERROR_BAD_PARAMETERS);
  TestExpectDiagnostic ("REE-FS open framing", 64, TEEC_ERROR_BAD_PARAMETERS);
  memset (mName, 'x', 64);
  Begin (OPTEE_RPC_FS_CREATE, 3, 0, 0);
  Mem (1, mName, 64, OPTEE_MSG_ATTR_TYPE_TMEM_INPUT);
  mArg->Params[2].Attr = OPTEE_MSG_ATTR_TYPE_VALUE_OUTPUT;
  assert (OpteeReeFsHandle (mArg) == TEEC_ERROR_BAD_PARAMETERS);
  TestExpectDiagnostic ("REE-FS open framing", 64, TEEC_ERROR_BAD_PARAMETERS);
  /* Four rounded 1MiB allocations exhaust the global capacity, not slots. */
  for (unsigned I = 0; I < 4; I++) {
    snprintf (Name, sizeof (Name), "q%u", I);
    assert (Open (Name, 1, &Handles[I]) == TEEC_SUCCESS);
    assert (Simple (OPTEE_RPC_FS_TRUNCATE, Handles[I], FILE_CAP) == TEEC_SUCCESS);
  }
  assert (Open ("extra", 1, &Other) == TEEC_SUCCESS);
  assert (Simple (OPTEE_RPC_FS_TRUNCATE, Other, 1) == TEEC_ERROR_OUT_OF_MEMORY);
  TestExpectDiagnostic ("REE-FS capacity", 1, TEEC_ERROR_OUT_OF_MEMORY);
  assert (Io (Other, 0, 1, 0) == TEEC_SUCCESS && mArg->Params[1].U.Tmem.Size == 0);
  /* A logical shrink does not refund retained rounded storage. */
  assert (Simple (OPTEE_RPC_FS_TRUNCATE, Handles[0], 0) == TEEC_SUCCESS);
  assert (Simple (OPTEE_RPC_FS_TRUNCATE, Other, 1) == TEEC_ERROR_OUT_OF_MEMORY);
  TestExpectDiagnostic ("REE-FS capacity", 1, TEEC_ERROR_OUT_OF_MEMORY);
  assert (Remove ("q0") == TEEC_SUCCESS);
  assert (Simple (OPTEE_RPC_FS_TRUNCATE, Other, 1) == TEEC_SUCCESS);
  for (unsigned I = 1; I < 4; I++) {
    snprintf (Name, sizeof (Name), "q%u", I); assert (Remove (Name) == TEEC_SUCCESS);
  }
  assert (Remove ("extra") == TEEC_SUCCESS);
  for (unsigned I = 0; I < 16; I++) {
    snprintf (Name, sizeof (Name), "s%u", I);
    assert (Open (Name, 1, &Handles[I]) == TEEC_SUCCESS);
  }
  assert (Open ("full", 1, &Other) == TEEC_ERROR_OUT_OF_MEMORY);
  TestExpectDiagnostic ("REE-FS file quota", 16, TEEC_ERROR_OUT_OF_MEMORY);
  for (unsigned I = 0; I < 16; I++) {
    snprintf (Name, sizeof (Name), "s%u", I); assert (Remove (Name) == TEEC_SUCCESS);
  }
  assert (Open ("fds", 1, &Handles[0]) == TEEC_SUCCESS);
  memcpy (mIo, "data", 4);
  assert (Io (Handles[0], 0, 4, 1) == TEEC_SUCCESS);
  for (unsigned I = 1; I < 64; I++) assert (Open ("fds", 0, &Handles[I]) == TEEC_SUCCESS);
  assert (Open ("fds", 1, &Other) == TEEC_ERROR_OUT_OF_MEMORY);
  TestExpectDiagnostic ("REE-FS descriptor quota", 64, TEEC_ERROR_OUT_OF_MEMORY);
  CheckData (Handles[0]);
  for (unsigned I = 0; I < 64; I++) {
    assert (Handles[I] > Last); Last = Handles[I];
    assert (Simple (OPTEE_RPC_FS_CLOSE, Handles[I], 0) == TEEC_SUCCESS);
  }
  assert (Open ("fds", 0, &Other) == TEEC_SUCCESS && Other > Last);
  Last = Other;
  /* Reset releases files/descriptors but must not recycle a descriptor ID. */
  TestFini ();
  TestInit ();
  mName = OpteeShmAllocOwned (64, 8, OPTEE_SHM_RPC_APPL);
  mIo = OpteeShmAllocOwned (4096, 8, OPTEE_SHM_RPC_APPL);
  assert (mName && mIo);
  assert (Open ("fds", 0, &Other) == TEEC_ERROR_ITEM_NOT_FOUND);
  TestExpectDiagnostic ("REE-FS file missing", 0, TEEC_ERROR_ITEM_NOT_FOUND);
  assert (Open ("fresh", 1, &Other) == TEEC_SUCCESS && Other > Last);
  assert (Io (Last, 0, 1, 0) != TEEC_SUCCESS);
  TestExpectDiagnostic ("REE-FS descriptor", Last, TEEC_ERROR_GENERIC);
}
int main (int argc, char **argv) {
  UINT64 Fd, Other;
  const char *Case = argc > 1 ? argv[1] : "closed";
  TestInit ();
  mName = OpteeShmAllocOwned (64, 8, OPTEE_SHM_RPC_APPL);
  mIo = OpteeShmAllocOwned (4096, 8, OPTEE_SHM_RPC_APPL);
  assert (mName && mIo);
  assert (Open ("a", 1, &Fd) == TEEC_SUCCESS);
  memcpy (mIo, "data", 4);
  assert (Io (Fd, 0, 4, 1) == TEEC_SUCCESS);
  if (strcmp (Case, "closed") == 0) {
    assert (Simple (OPTEE_RPC_FS_CLOSE, Fd, 0) == TEEC_SUCCESS);
    assert (Io (Fd, 0, 4, 0) != TEEC_SUCCESS);
    TestExpectDiagnostic ("REE-FS descriptor", Fd, TEEC_ERROR_GENERIC);
    assert (Simple (OPTEE_RPC_FS_CLOSE, Fd, 0) != TEEC_SUCCESS);
    TestExpectDiagnostic ("REE-FS descriptor", Fd, TEEC_ERROR_GENERIC);
    assert (Open ("a", 0, &Other) == TEEC_SUCCESS && Other > Fd);
    CheckData (Other);
  } else if (strcmp (Case, "stale") == 0) {
    assert (Remove ("a") == TEEC_SUCCESS);
    assert (Open ("a", 1, &Other) == TEEC_SUCCESS && Other > Fd);
    assert (Io (Fd, 0, 4, 0) != TEEC_SUCCESS);
    TestExpectDiagnostic ("REE-FS descriptor", Fd, TEEC_ERROR_GENERIC);
  } else if (strcmp (Case, "huge") == 0) {
    assert (Simple (OPTEE_RPC_FS_TRUNCATE, Fd, UINT64_MAX) != TEEC_SUCCESS);
    TestExpectDiagnostic ("REE-FS capacity", UINT64_MAX, TEEC_ERROR_OUT_OF_MEMORY);
    CheckData (Fd);
    assert (Simple (OPTEE_RPC_FS_TRUNCATE, Fd, FILE_CAP + 1) != TEEC_SUCCESS);
    TestExpectDiagnostic ("REE-FS capacity", FILE_CAP + 1, TEEC_ERROR_OUT_OF_MEMORY);
    CheckData (Fd);
  } else if (strcmp (Case, "direction") == 0) {
    Begin (OPTEE_RPC_FS_WRITE, 2, Fd, 0);
    memcpy (mIo, "evil", 4);
    Mem (1, mIo, 4, OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT);
    assert (OpteeReeFsHandle (mArg) == TEEC_ERROR_BAD_PARAMETERS);
    TestExpectDiagnostic ("REE-FS IO framing", OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT, TEEC_ERROR_BAD_PARAMETERS);
    CheckData (Fd);
    Begin (OPTEE_RPC_FS_TRUNCATE, 2, Fd, 0);
    assert (OpteeReeFsHandle (mArg) == TEEC_ERROR_BAD_PARAMETERS);
    TestExpectDiagnostic ("REE-FS truncate framing", 2, TEEC_ERROR_BAD_PARAMETERS);
    CheckData (Fd);
    Directions (Fd);
  } else if (strcmp (Case, "sequence") == 0) {
    Sequences (Fd);
  } else { assert (!"unknown test case"); }
  TestFini ();
  puts ("REE FS regression passed");
  return 0;
}
