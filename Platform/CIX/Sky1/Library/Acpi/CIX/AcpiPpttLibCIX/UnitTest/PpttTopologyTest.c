/** @file
  Sky1 PPTT layout, read back with the lookup rules of Linux
  drivers/acpi/pptt.c: a cache belongs to the processor node whose private
  resources lead to it, and its level is the chain depth plus the depth of
  the caches below that node.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdlib.h>
#include <cmocka.h>
#include "PpttTopology.h"

#define HEADER_SIZE  sizeof (EFI_ACPI_DESCRIPTION_HEADER)
#define TYPE_DATA     EFI_ACPI_6_3_CACHE_ATTRIBUTES_CACHE_TYPE_DATA
#define TYPE_INSTR    EFI_ACPI_6_3_CACHE_ATTRIBUTES_CACHE_TYPE_INSTRUCTION
#define TYPE_UNIFIED  EFI_ACPI_6_3_CACHE_ATTRIBUTES_CACHE_TYPE_UNIFIED

typedef struct {
  UINT8     *Table;
  UINT32    Size;
} PPTT;

//
// The live O6 topology: the boot pair (physical cores 10, 11) gets UIDs 0-1,
// the Cortex-A520 cluster (physical 0-3) UIDs 2-5, then physical 4-9.
//
STATIC CIX_CPU_CORE  mBootPair[] = {
  { 10, 0, TRUE }, { 11, 1, TRUE }
};
STATIC CIX_CPU_CORE  mLittle[] = {
  { 0, 2, TRUE }, { 1, 3, TRUE }, { 2, 4, TRUE }, { 3, 5, TRUE }
};
STATIC CIX_CPU_CORE  mMid0[] = {
  { 4, 6, TRUE }, { 5, 7, TRUE }
};
STATIC CIX_CPU_CORE  mMid1[] = {
  { 6, 8, TRUE }, { 7, 9, TRUE }
};
STATIC CIX_CPU_CORE  mBig[] = {
  { 8, 10, TRUE }, { 9, 11, TRUE }
};
STATIC CIX_CLUSTER_TOPO  mClusters[] = {
  { mBootPair, 1, 2 }, { mLittle, 2, 4 }, { mMid0, 3, 2 }, { mMid1, 4, 2 }, { mBig, 5, 2 }
};
STATIC CM_CIX_CPU_TOPO_INFO  mTopo = { mClusters, 5 };

STATIC VOID
ResetTopology (
  VOID
  )
{
  UINT32  Cluster;
  UINT32  Core;

  for (Cluster = 0; Cluster < mTopo.ClusterNumber; Cluster++) {
    for (Core = 0; Core < mClusters[Cluster].CoreNumber; Core++) {
      mClusters[Cluster].Core[Core].Enable = TRUE;
    }
  }
}

STATIC PPTT
Build (
  BOOLEAN  DescribeCaches
  )
{
  PPTT  Pptt;

  Pptt.Size  = Sky1PpttSize (&mTopo, DescribeCaches);
  Pptt.Table = calloc (1, Pptt.Size);
  assert_non_null (Pptt.Table);
  Sky1PpttWriteNodes (&mTopo, DescribeCaches, Pptt.Table);
  return Pptt;
}

STATIC CONST EFI_ACPI_6_3_PPTT_STRUCTURE_PROCESSOR *
ProcessorAt (
  CONST PPTT  *Pptt,
  UINT32      Offset
  )
{
  CONST EFI_ACPI_6_3_PPTT_STRUCTURE_PROCESSOR  *Node;

  assert_true (Offset >= HEADER_SIZE);
  assert_true (Offset + sizeof (*Node) <= Pptt->Size);
  Node = (CONST VOID *)(Pptt->Table + Offset);
  assert_int_equal (Node->Type, EFI_ACPI_6_3_PPTT_TYPE_PROCESSOR);
  assert_int_equal (Node->Length, sizeof (*Node) + Node->NumberOfPrivateResources * sizeof (UINT32));
  assert_true (Offset + Node->Length <= Pptt->Size);
  return Node;
}

STATIC CONST EFI_ACPI_6_3_PPTT_STRUCTURE_CACHE *
CacheAt (
  CONST PPTT  *Pptt,
  UINT32      Offset
  )
{
  CONST EFI_ACPI_6_3_PPTT_STRUCTURE_CACHE  *Cache;

  assert_true (Offset >= HEADER_SIZE);
  assert_true (Offset + sizeof (*Cache) <= Pptt->Size);
  Cache = (CONST VOID *)(Pptt->Table + Offset);
  assert_int_equal (Cache->Type, EFI_ACPI_6_3_PPTT_TYPE_CACHE);
  assert_int_equal (Cache->Length, sizeof (*Cache));
  return Cache;
}

STATIC UINT32
Resource (
  CONST EFI_ACPI_6_3_PPTT_STRUCTURE_PROCESSOR  *Node,
  UINT32                                       Index
  )
{
  return ((CONST UINT32 *)(Node + 1))[Index];
}

/** Walk every structure; they must tile the table exactly. */
STATIC VOID
CheckTiling (
  CONST PPTT  *Pptt
  )
{
  UINT32  Offset;

  for (Offset = HEADER_SIZE; Offset < Pptt->Size; Offset += Pptt->Table[Offset + 1]) {
    assert_true (Pptt->Table[Offset] <= EFI_ACPI_6_3_PPTT_TYPE_CACHE);
    assert_int_not_equal (Pptt->Table[Offset + 1], 0);
  }

  assert_int_equal (Offset, Pptt->Size);
}

/** Return the leaf node of a UID, or 0 if none. */
STATIC UINT32
FindLeaf (
  CONST PPTT  *Pptt,
  UINT32      Uid
  )
{
  UINT32                                       Offset;
  UINT32                                       Found;
  CONST EFI_ACPI_6_3_PPTT_STRUCTURE_PROCESSOR  *Node;

  Found = 0;
  for (Offset = HEADER_SIZE; Offset < Pptt->Size; Offset += Pptt->Table[Offset + 1]) {
    if (Pptt->Table[Offset] != EFI_ACPI_6_3_PPTT_TYPE_PROCESSOR) {
      continue;
    }

    Node = ProcessorAt (Pptt, Offset);
    if ((Node->AcpiProcessorId == Uid) && Node->Flags.NodeIsALeaf) {
      assert_int_equal (Found, 0);
      Found = Offset;
    }
  }

  return Found;
}

STATIC BOOLEAN
TypeMatches (
  CONST EFI_ACPI_6_3_PPTT_STRUCTURE_CACHE  *Cache,
  UINT8                                    Type
  )
{
  if (Type == TYPE_UNIFIED) {
    return (Cache->Attributes.CacheType & TYPE_UNIFIED) != 0;
  }

  return Cache->Attributes.CacheType == Type;
}

/**
  Linux acpi_find_cache_node (): search the leaf, then each ancestor; the
  levels of an ancestor's caches start above the deepest chain below it.
  Level 0 only counts levels, returned through Levels.
**/
STATIC CONST EFI_ACPI_6_3_PPTT_STRUCTURE_CACHE *
FindCache (
  CONST PPTT  *Pptt,
  UINT32      Uid,
  UINT32      Level,
  UINT8       Type,
  UINT32      *Owner,
  UINT32      *Levels
  )
{
  CONST EFI_ACPI_6_3_PPTT_STRUCTURE_PROCESSOR  *Node;
  CONST EFI_ACPI_6_3_PPTT_STRUCTURE_CACHE      *Cache;
  CONST EFI_ACPI_6_3_PPTT_STRUCTURE_CACHE      *Found;
  UINT32                                       NodeOffset;
  UINT32                                       Total;
  UINT32                                       Deepest;
  UINT32                                       Local;
  UINT32                                       Index;

  Found      = NULL;
  Total      = 0;
  NodeOffset = FindLeaf (Pptt, Uid);
  assert_int_not_equal (NodeOffset, 0);
  while (NodeOffset != 0) {
    Node    = ProcessorAt (Pptt, NodeOffset);
    Deepest = Total;
    for (Index = 0; Index < Node->NumberOfPrivateResources; Index++) {
      Local = Total;
      for (Cache = CacheAt (Pptt, Resource (Node, Index)); Cache != NULL;
           Cache = (Cache->NextLevelOfCache != 0) ? CacheAt (Pptt, Cache->NextLevelOfCache) : NULL)
      {
        Local++;
        if ((Local == Level) && TypeMatches (Cache, Type)) {
          // Linux warns about, and cannot resolve, a duplicate level/type.
          assert_true ((Found == NULL) || (Found == Cache));
          Found = Cache;
        }
      }

      if (Local > Deepest) {
        Deepest = Local;
      }
    }

    Total = Deepest;
    if (Found != NULL) {
      *Owner = NodeOffset;
      break;
    }

    NodeOffset = Node->Parent;
  }

  if (Levels != NULL) {
    *Levels = Total;
  }

  return Found;
}

STATIC VOID
CheckCache (
  CONST PPTT                 *Pptt,
  UINT32                     Uid,
  UINT32                     Level,
  UINT8                      Type,
  CONST SKY1_CACHE_GEOMETRY  *Expected,
  UINT32                     ExpectedOwner
  )
{
  CONST EFI_ACPI_6_3_PPTT_STRUCTURE_CACHE  *Cache;
  UINT32                                   Owner;

  Owner = 0;
  Cache = FindCache (Pptt, Uid, Level, Type, &Owner, NULL);
  assert_non_null (Cache);
  assert_int_equal (Owner, ExpectedOwner);
  assert_int_equal (Cache->Size, Expected->Size);
  assert_int_equal (Cache->NumberOfSets, Expected->NumberOfSets);
  assert_int_equal (Cache->Associativity, Expected->Associativity);
  assert_int_equal (Cache->LineSize, 64);
  assert_true (Cache->Flags.SizePropertyValid && Cache->Flags.NumberOfSetsValid);
  assert_true (Cache->Flags.AssociativityValid && Cache->Flags.LineSizeValid);
  assert_true (Cache->Flags.CacheTypeValid);
  assert_false (Cache->Flags.AllocationTypeValid || Cache->Flags.WritePolicyValid);
}

STATIC UINT32
LevelCount (
  CONST PPTT  *Pptt,
  UINT32      Uid
  )
{
  UINT32  Levels;
  UINT32  Owner;

  assert_null (FindCache (Pptt, Uid, 0, TYPE_UNIFIED, &Owner, &Levels));
  return Levels;
}

/** Check one enabled core: private L1 (and L2), shared cache at the socket. */
STATIC VOID
CheckCore (
  CONST PPTT          *Pptt,
  CONST CIX_CPU_CORE  *Core
  )
{
  CONST SKY1_CORE_CACHES  *Caches;
  UINT32                  Leaf;
  UINT32                  SharedLevel;

  Caches = (Core->Coreid < 4) ? &gSky1CortexA520Caches : &gSky1CortexA720Caches;
  Leaf   = FindLeaf (Pptt, Core->Uid);
  CheckCache (Pptt, Core->Uid, 1, TYPE_DATA, &Caches->L1, Leaf);
  CheckCache (Pptt, Core->Uid, 1, TYPE_INSTR, &Caches->L1, Leaf);
  if (Caches->L2.Size != 0) {
    CheckCache (Pptt, Core->Uid, 2, TYPE_UNIFIED, &Caches->L2, Leaf);
  }

  // Same level count as the core's CLIDR_EL1, so no phantom or missing level.
  SharedLevel = (Caches->L2.Size != 0) ? 3 : 2;
  CheckCache (Pptt, Core->Uid, SharedLevel, TYPE_UNIFIED, &gSky1SharedCache, HEADER_SIZE);
  assert_int_equal (LevelCount (Pptt, Core->Uid), SharedLevel);
}

STATIC VOID
CheckHierarchy (
  CONST PPTT  *Pptt
  )
{
  UINT32                                       Cluster;
  UINT32                                       Core;
  UINT32                                       Leaf;
  CONST CIX_CLUSTER_TOPO                       *Topo;
  CONST EFI_ACPI_6_3_PPTT_STRUCTURE_PROCESSOR  *Parent;

  CheckTiling (Pptt);
  assert_int_equal (ProcessorAt (Pptt, HEADER_SIZE)->Parent, 0);
  for (Cluster = 0; Cluster < mTopo.ClusterNumber; Cluster++) {
    Topo = &mClusters[Cluster];
    for (Core = 0; Core < Topo->CoreNumber; Core++) {
      Leaf = FindLeaf (Pptt, Topo->Core[Core].Uid);
      if (!Topo->Core[Core].Enable) {
        assert_int_equal (Leaf, 0);
        continue;
      }

      Parent = ProcessorAt (Pptt, ProcessorAt (Pptt, Leaf)->Parent);
      assert_int_equal (Parent->AcpiProcessorId, Topo->Uid);
      assert_false (Parent->Flags.NodeIsALeaf);
      assert_int_equal (Parent->Parent, HEADER_SIZE);
    }
  }
}

STATIC BOOLEAN
ClusterNodeExists (
  CONST PPTT  *Pptt,
  UINT32      Uid
  )
{
  UINT32                                       Offset;
  CONST EFI_ACPI_6_3_PPTT_STRUCTURE_PROCESSOR  *Node;

  for (Offset = HEADER_SIZE; Offset < Pptt->Size; Offset += Pptt->Table[Offset + 1]) {
    if (Pptt->Table[Offset] == EFI_ACPI_6_3_PPTT_TYPE_PROCESSOR) {
      Node = ProcessorAt (Pptt, Offset);
      if ((Node->Parent == HEADER_SIZE) && (Node->AcpiProcessorId == Uid)) {
        return TRUE;
      }
    }
  }

  return FALSE;
}

STATIC VOID
AllCoresDescribed (
  VOID  **State
  )
{
  PPTT    Pptt;
  UINT32  Cluster;
  UINT32  Core;

  (VOID)State;
  ResetTopology ();
  Pptt = Build (TRUE);
  CheckHierarchy (&Pptt);
  for (Cluster = 0; Cluster < mTopo.ClusterNumber; Cluster++) {
    for (Core = 0; Core < mClusters[Cluster].CoreNumber; Core++) {
      CheckCore (&Pptt, &mClusters[Cluster].Core[Core]);
    }
  }

  free (Pptt.Table);
}

STATIC VOID
DisabledCoresOmitted (
  VOID  **State
  )
{
  PPTT    Pptt;
  UINT32  Core;

  (VOID)State;
  ResetTopology ();
  mBootPair[1].Enable = FALSE;
  for (Core = 0; Core < 4; Core++) {
    mLittle[Core].Enable = FALSE;
  }

  Pptt = Build (TRUE);
  CheckHierarchy (&Pptt);
  assert_false (ClusterNodeExists (&Pptt, 2));
  assert_true (ClusterNodeExists (&Pptt, 1));
  CheckCore (&Pptt, &mBootPair[0]);
  CheckCore (&Pptt, &mMid0[1]);
  CheckCore (&Pptt, &mBig[1]);
  free (Pptt.Table);
  ResetTopology ();
}

STATIC VOID
WithoutCachesProcessorsOnly (
  VOID  **State
  )
{
  PPTT    Pptt;
  UINT32  Offset;
  UINT32  Owner;

  (VOID)State;
  ResetTopology ();
  Pptt = Build (FALSE);
  assert_int_equal (Pptt.Size, HEADER_SIZE + (1 + 5 + 12) * sizeof (EFI_ACPI_6_3_PPTT_STRUCTURE_PROCESSOR));
  CheckHierarchy (&Pptt);
  for (Offset = HEADER_SIZE; Offset < Pptt.Size; Offset += Pptt.Table[Offset + 1]) {
    assert_int_equal (ProcessorAt (&Pptt, Offset)->NumberOfPrivateResources, 0);
  }

  assert_null (FindCache (&Pptt, 0, 1, TYPE_DATA, &Owner, NULL));
  free (Pptt.Table);
}

/** CCSIDR_EL1/MIDR_EL1 values as read on Sky1 Cortex-A720 and Cortex-A520 cores. */
STATIC VOID
BootCoreRegistersMatch (
  VOID  **State
  )
{
  (VOID)State;
  assert_ptr_equal (Sky1CoreCachesForMidr (0x410FD811), &gSky1CortexA720Caches);
  assert_ptr_equal (Sky1CoreCachesForMidr (0x410FD801), &gSky1CortexA520Caches);
  assert_null (Sky1CoreCachesForMidr (0x410FD4F0));
  assert_null (Sky1CoreCachesForMidr (0x610FD811));

  assert_true (Sky1CcsidrMatches (0x000000FF0000001AULL, TRUE, &gSky1CortexA720Caches.L1));
  assert_true (Sky1CcsidrMatches (0x000003FF0000003AULL, TRUE, &gSky1CortexA720Caches.L2));
  assert_true (Sky1CcsidrMatches (0x00003FFF0000005AULL, TRUE, &gSky1SharedCache));
  assert_true (Sky1CcsidrMatches (0x0000007F0000001AULL, TRUE, &gSky1CortexA520Caches.L1));
  assert_false (Sky1CcsidrMatches (0x000000FF0000001AULL, TRUE, &gSky1CortexA520Caches.L1));
  assert_false (Sky1CcsidrMatches (0x000003FF0000003AULL, TRUE, &gSky1SharedCache));
  // A 1 MiB 16-way cache (the size other firmware labels as the 12 MiB L3).
  assert_false (Sky1CcsidrMatches (0x000003FF0000007AULL, TRUE, &gSky1SharedCache));
  // 32-bit layout: 64 KiB, 4-way, 256 sets, 64-byte lines.
  assert_true (Sky1CcsidrMatches ((255ULL << 13) | (3 << 3) | 2, FALSE, &gSky1CortexA720Caches.L1));
  assert_false (Sky1CcsidrMatches ((255ULL << 13) | (3 << 3) | 1, FALSE, &gSky1CortexA720Caches.L1));
}

int
main (
  void
  )
{
  const struct CMUnitTest  Tests[] = {
    cmocka_unit_test (AllCoresDescribed),
    cmocka_unit_test (DisabledCoresOmitted),
    cmocka_unit_test (WithoutCachesProcessorsOnly),
    cmocka_unit_test (BootCoreRegistersMatch)
  };

  return cmocka_run_group_tests (Tests, NULL, NULL);
}
