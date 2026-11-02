/** @file
  Sky1 PPTT processor hierarchy and cache layout.

  The cache geometry is the one the Sky1 cores report in CLIDR_EL1 and
  CCSIDR_EL1 (FEAT_CCIDX layout):

    Cortex-A720  CLIDR Ctype 0x123: L1I/L1D 64 KiB 4-way 256 sets,
                 L2 512 KiB 8-way 1024 sets, L3 12 MiB 12-way 16384 sets.
    Cortex-A520  CLIDR Ctype 0x023: L1I/L1D 32 KiB 4-way 128 sets,
                 L2 12 MiB 12-way 16384 sets.

  The 12 MiB cache is the DSU L3 shared by every core. The Cortex-A520 cores
  have no private L2, so they see it as their level 2. All lines are 64 bytes.

  SPDX-License-Identifier: BSD-2-Clause-Patent

  @par Reference(s):
  - ACPI 6.3 Specification, 5.2.29 Processor Properties Topology Table
  - Arm Architecture Reference Manual, CLIDR_EL1 and CCSIDR_EL1
**/

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>

#include "PpttTopology.h"

#define SKY1_CACHE_LINE_SIZE  64

#define ARM_IMPLEMENTER_ARM   0x41
#define CORTEX_A520_PART_NUM  0xD80
#define CORTEX_A720_PART_NUM  0xD81

#define PPTT_PROCESSOR_SIZE(Resources) \
  (sizeof (EFI_ACPI_6_3_PPTT_STRUCTURE_PROCESSOR) + (Resources) * sizeof (UINT32))
#define PPTT_CACHE_SIZE  sizeof (EFI_ACPI_6_3_PPTT_STRUCTURE_CACHE)

CONST SKY1_CORE_CACHES  gSky1CortexA720Caches = {
  CORTEX_A720_PART_NUM,
  0x123,
  { SIZE_64KB,  256,  4 },
  { SIZE_512KB, 1024, 8 }
};

CONST SKY1_CORE_CACHES  gSky1CortexA520Caches = {
  CORTEX_A520_PART_NUM,
  0x023,
  { SIZE_32KB, 128, 4 },
  { 0,         0,   0 }
};

CONST SKY1_CACHE_GEOMETRY  gSky1SharedCache = { 12 * SIZE_1MB, 16384, 12 };

CONST SKY1_CORE_CACHES *
Sky1CoreCachesForMidr (
  IN UINT64  Midr
  )
{
  if (((Midr >> 24) & 0xFF) != ARM_IMPLEMENTER_ARM) {
    return NULL;
  }

  switch ((Midr >> 4) & 0xFFF) {
    case CORTEX_A720_PART_NUM:
      return &gSky1CortexA720Caches;
    case CORTEX_A520_PART_NUM:
      return &gSky1CortexA520Caches;
    default:
      return NULL;
  }
}

BOOLEAN
Sky1CcsidrMatches (
  IN       UINT64               Ccsidr,
  IN       BOOLEAN              Ccidx,
  IN CONST SKY1_CACHE_GEOMETRY  *Expected
  )
{
  UINT64  LineSize;
  UINT64  Ways;
  UINT64  Sets;

  LineSize = 1ULL << ((Ccsidr & 0x7) + 4);
  if (Ccidx) {
    Ways = ((Ccsidr >> 3) & 0x1FFFFF) + 1;
    Sets = ((Ccsidr >> 32) & 0xFFFFFF) + 1;
  } else {
    Ways = ((Ccsidr >> 3) & 0x3FF) + 1;
    Sets = ((Ccsidr >> 13) & 0x7FFF) + 1;
  }

  return (LineSize == SKY1_CACHE_LINE_SIZE) &&
         (Ways == Expected->Associativity) &&
         (Sets == Expected->NumberOfSets) &&
         (LineSize * Ways * Sets == Expected->Size);
}

/**
  Physical cores 0-3 (MPIDR Aff1 0-3) are the Cortex-A520 cores, the others
  are Cortex-A720.
**/
STATIC
CONST SKY1_CORE_CACHES *
CoreCaches (
  IN CONST CIX_CPU_CORE  *Core
  )
{
  return (Core->Coreid < PLAT_CPU_L0_COUNT) ? &gSky1CortexA520Caches : &gSky1CortexA720Caches;
}

STATIC
UINT32
CoreCacheCount (
  IN CONST SKY1_CORE_CACHES  *Caches
  )
{
  return (Caches->L2.Size != 0) ? 3 : 2;
}

STATIC
BOOLEAN
ClusterHasEnabledCore (
  IN CONST CIX_CLUSTER_TOPO  *Cluster
  )
{
  UINT32  CoreIndex;

  for (CoreIndex = 0; CoreIndex < Cluster->CoreNumber; CoreIndex++) {
    if (Cluster->Core[CoreIndex].Enable) {
      return TRUE;
    }
  }

  return FALSE;
}

UINT32
Sky1PpttSize (
  IN CONST CM_CIX_CPU_TOPO_INFO  *CpuTopo,
  IN       BOOLEAN               DescribeCaches
  )
{
  UINT32                  Size;
  UINT32                  ClusterIndex;
  UINT32                  CoreIndex;
  CONST CIX_CLUSTER_TOPO  *Cluster;
  CONST CIX_CPU_CORE      *Core;

  Size = sizeof (EFI_ACPI_DESCRIPTION_HEADER);
  Size += DescribeCaches ? PPTT_PROCESSOR_SIZE (1) + PPTT_CACHE_SIZE : PPTT_PROCESSOR_SIZE (0);

  for (ClusterIndex = 0; ClusterIndex < CpuTopo->ClusterNumber; ClusterIndex++) {
    Cluster = &CpuTopo->ClusterTopo[ClusterIndex];
    if (!ClusterHasEnabledCore (Cluster)) {
      continue;
    }

    Size += PPTT_PROCESSOR_SIZE (0);
    for (CoreIndex = 0; CoreIndex < Cluster->CoreNumber; CoreIndex++) {
      Core = &Cluster->Core[CoreIndex];
      if (!Core->Enable) {
        continue;
      }

      if (DescribeCaches) {
        Size += PPTT_PROCESSOR_SIZE (2) + CoreCacheCount (CoreCaches (Core)) * PPTT_CACHE_SIZE;
      } else {
        Size += PPTT_PROCESSOR_SIZE (0);
      }
    }
  }

  return Size;
}

STATIC
UINT32
AppendProcessor (
  IN OUT   UINT8                                        *Table,
  IN       UINT32                                       Offset,
  IN       EFI_ACPI_6_3_PPTT_STRUCTURE_PROCESSOR_FLAGS  Flags,
  IN       UINT32                                       Parent,
  IN       UINT32                                       AcpiProcessorId,
  IN       UINT32                                       ResourceCount,
  IN CONST UINT32                                       *Resources
  )
{
  EFI_ACPI_6_3_PPTT_STRUCTURE_PROCESSOR  Node;

  ZeroMem (&Node, sizeof (Node));
  Node.Type                     = EFI_ACPI_6_3_PPTT_TYPE_PROCESSOR;
  Node.Length                   = (UINT8)PPTT_PROCESSOR_SIZE (ResourceCount);
  Node.Flags                    = Flags;
  Node.Parent                   = Parent;
  Node.AcpiProcessorId          = AcpiProcessorId;
  Node.NumberOfPrivateResources = ResourceCount;

  CopyMem (Table + Offset, &Node, sizeof (Node));
  if (ResourceCount != 0) {
    CopyMem (Table + Offset + sizeof (Node), Resources, ResourceCount * sizeof (UINT32));
  }

  return Offset + Node.Length;
}

STATIC
UINT32
AppendCache (
  IN OUT   UINT8                *Table,
  IN       UINT32               Offset,
  IN CONST SKY1_CACHE_GEOMETRY  *Geometry,
  IN       UINT8                CacheType,
  IN       UINT32               NextLevelOfCache
  )
{
  EFI_ACPI_6_3_PPTT_STRUCTURE_CACHE  Cache;

  ZeroMem (&Cache, sizeof (Cache));
  Cache.Type   = EFI_ACPI_6_3_PPTT_TYPE_CACHE;
  Cache.Length = (UINT8)PPTT_CACHE_SIZE;
  //
  // Allocation and write policy are not reported by CCSIDR_EL1 with
  // FEAT_CCIDX, so they stay invalid.
  //
  Cache.Flags.SizePropertyValid  = EFI_ACPI_6_3_PPTT_CACHE_SIZE_VALID;
  Cache.Flags.NumberOfSetsValid  = EFI_ACPI_6_3_PPTT_NUMBER_OF_SETS_VALID;
  Cache.Flags.AssociativityValid = EFI_ACPI_6_3_PPTT_ASSOCIATIVITY_VALID;
  Cache.Flags.CacheTypeValid     = EFI_ACPI_6_3_PPTT_CACHE_TYPE_VALID;
  Cache.Flags.LineSizeValid      = EFI_ACPI_6_3_PPTT_LINE_SIZE_VALID;
  Cache.NextLevelOfCache         = NextLevelOfCache;
  Cache.Size                     = Geometry->Size;
  Cache.NumberOfSets             = Geometry->NumberOfSets;
  Cache.Associativity            = Geometry->Associativity;
  Cache.Attributes.CacheType     = CacheType;
  Cache.LineSize                 = SKY1_CACHE_LINE_SIZE;

  CopyMem (Table + Offset, &Cache, sizeof (Cache));
  return Offset + Cache.Length;
}

/**
  Append a core node followed by its private caches: L1D, L1I and, for core
  types that have one, the L2 both L1 caches lead to.
**/
STATIC
UINT32
AppendCoreWithCaches (
  IN OUT   UINT8                                        *Table,
  IN       UINT32                                       Offset,
  IN       EFI_ACPI_6_3_PPTT_STRUCTURE_PROCESSOR_FLAGS  Flags,
  IN       UINT32                                       Parent,
  IN CONST CIX_CPU_CORE                                 *Core
  )
{
  CONST SKY1_CORE_CACHES  *Caches;
  UINT32                  L1D;
  UINT32                  L1I;
  UINT32                  L2;
  UINT32                  Resources[2];

  Caches       = CoreCaches (Core);
  L1D          = Offset + PPTT_PROCESSOR_SIZE (2);
  L1I          = L1D + PPTT_CACHE_SIZE;
  L2           = (Caches->L2.Size != 0) ? L1I + PPTT_CACHE_SIZE : 0;
  Resources[0] = L1D;
  Resources[1] = L1I;

  Offset = AppendProcessor (Table, Offset, Flags, Parent, Core->Uid, 2, Resources);
  Offset = AppendCache (Table, Offset, &Caches->L1, EFI_ACPI_6_3_CACHE_ATTRIBUTES_CACHE_TYPE_DATA, L2);
  Offset = AppendCache (Table, Offset, &Caches->L1, EFI_ACPI_6_3_CACHE_ATTRIBUTES_CACHE_TYPE_INSTRUCTION, L2);
  if (L2 != 0) {
    Offset = AppendCache (Table, Offset, &Caches->L2, EFI_ACPI_6_3_CACHE_ATTRIBUTES_CACHE_TYPE_UNIFIED, 0);
  }

  return Offset;
}

VOID
Sky1PpttWriteNodes (
  IN CONST CM_CIX_CPU_TOPO_INFO  *CpuTopo,
  IN       BOOLEAN               DescribeCaches,
  OUT      UINT8                 *Table
  )
{
  UINT32                  Offset;
  UINT32                  SocketOffset;
  UINT32                  ClusterOffset;
  UINT32                  SharedCache;
  UINT32                  ClusterIndex;
  UINT32                  CoreIndex;
  CONST CIX_CLUSTER_TOPO  *Cluster;
  CONST CIX_CPU_CORE      *Core;

  EFI_ACPI_6_3_PPTT_STRUCTURE_PROCESSOR_FLAGS  SocketFlags = {
    EFI_ACPI_6_3_PPTT_PACKAGE_PHYSICAL,
    EFI_ACPI_6_3_PPTT_PROCESSOR_ID_VALID,
    EFI_ACPI_6_3_PPTT_PROCESSOR_IS_NOT_THREAD,
    EFI_ACPI_6_3_PPTT_NODE_IS_NOT_LEAF,
    EFI_ACPI_6_3_PPTT_IMPLEMENTATION_NOT_IDENTICAL
  };

  EFI_ACPI_6_3_PPTT_STRUCTURE_PROCESSOR_FLAGS  ClusterFlags = {
    EFI_ACPI_6_3_PPTT_PACKAGE_NOT_PHYSICAL,
    EFI_ACPI_6_3_PPTT_PROCESSOR_ID_VALID,
    EFI_ACPI_6_3_PPTT_PROCESSOR_IS_NOT_THREAD,
    EFI_ACPI_6_3_PPTT_NODE_IS_NOT_LEAF,
    EFI_ACPI_6_3_PPTT_IMPLEMENTATION_IDENTICAL
  };

  EFI_ACPI_6_3_PPTT_STRUCTURE_PROCESSOR_FLAGS  CoreFlags = {
    EFI_ACPI_6_3_PPTT_PACKAGE_NOT_PHYSICAL,
    EFI_ACPI_6_3_PPTT_PROCESSOR_ID_VALID,
    EFI_ACPI_6_3_PPTT_PROCESSOR_IS_NOT_THREAD,
    EFI_ACPI_6_3_PPTT_NODE_IS_LEAF,
    EFI_ACPI_6_3_PPTT_IMPLEMENTATION_IDENTICAL
  };

  SocketOffset = sizeof (EFI_ACPI_DESCRIPTION_HEADER);
  if (DescribeCaches) {
    //
    // The shared cache hangs off the socket: Linux attributes a cache to the
    // processor node that references it, so this is what makes it shared.
    //
    SharedCache = SocketOffset + PPTT_PROCESSOR_SIZE (1);
    Offset      = AppendProcessor (Table, SocketOffset, SocketFlags, 0, 0, 1, &SharedCache);
    Offset      = AppendCache (Table, Offset, &gSky1SharedCache, EFI_ACPI_6_3_CACHE_ATTRIBUTES_CACHE_TYPE_UNIFIED, 0);
  } else {
    Offset = AppendProcessor (Table, SocketOffset, SocketFlags, 0, 0, 0, NULL);
  }

  for (ClusterIndex = 0; ClusterIndex < CpuTopo->ClusterNumber; ClusterIndex++) {
    Cluster = &CpuTopo->ClusterTopo[ClusterIndex];
    if (!ClusterHasEnabledCore (Cluster)) {
      continue;
    }

    ClusterOffset = Offset;
    Offset        = AppendProcessor (Table, Offset, ClusterFlags, SocketOffset, Cluster->Uid, 0, NULL);
    for (CoreIndex = 0; CoreIndex < Cluster->CoreNumber; CoreIndex++) {
      Core = &Cluster->Core[CoreIndex];
      if (!Core->Enable) {
        continue;
      }

      if (DescribeCaches) {
        Offset = AppendCoreWithCaches (Table, Offset, CoreFlags, ClusterOffset, Core);
      } else {
        Offset = AppendProcessor (Table, Offset, CoreFlags, ClusterOffset, Core->Uid, 0, NULL);
      }
    }
  }
}
