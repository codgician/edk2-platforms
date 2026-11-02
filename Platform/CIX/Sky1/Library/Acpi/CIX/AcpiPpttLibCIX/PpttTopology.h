/** @file
  Sky1 PPTT processor hierarchy and cache layout.

  SPDX-License-Identifier: BSD-2-Clause-Patent

  @par Reference(s):
  - ACPI 6.3 Specification, 5.2.29 Processor Properties Topology Table
  - Arm Architecture Reference Manual, CLIDR_EL1 and CCSIDR_EL1
**/

#ifndef PPTT_TOPOLOGY_H_
#define PPTT_TOPOLOGY_H_

#include <Library/AcpiNameSpaceObjects.h>

/** Geometry of one cache. Every Sky1 cache uses 64-byte lines. */
typedef struct {
  UINT32    Size;
  UINT32    NumberOfSets;
  UINT8     Associativity;
} SKY1_CACHE_GEOMETRY;

/** Caches private to one core of a given type. */
typedef struct {
  UINT16                 PartNum;    ///< MIDR_EL1.PartNum of the core type.
  UINT32                 ClidrCtype; ///< CLIDR_EL1.Ctype1..Ctype7 as read on the core.
  SKY1_CACHE_GEOMETRY    L1;         ///< L1 instruction and data caches (same geometry).
  SKY1_CACHE_GEOMETRY    L2;         ///< Private L2; Size 0 if the core has none.
} SKY1_CORE_CACHES;

extern CONST SKY1_CORE_CACHES     gSky1CortexA720Caches;
extern CONST SKY1_CORE_CACHES     gSky1CortexA520Caches;
extern CONST SKY1_CACHE_GEOMETRY  gSky1SharedCache;

/**
  Return the cache description for the core type identified by MIDR_EL1.

  @param [in] Midr  MIDR_EL1 value.

  @return The core type's caches, or NULL for a core that is not on Sky1.
**/
CONST SKY1_CORE_CACHES *
Sky1CoreCachesForMidr (
  IN UINT64  Midr
  );

/**
  Check a CCSIDR_EL1 value against the expected cache geometry.

  @param [in] Ccsidr    CCSIDR_EL1 value read for the cache.
  @param [in] Ccidx     TRUE if FEAT_CCIDX (64-bit CCSIDR_EL1 layout) is implemented.
  @param [in] Expected  Expected geometry.

  @retval TRUE   Line size, associativity, set count and size all match.
  @retval FALSE  Otherwise.
**/
BOOLEAN
Sky1CcsidrMatches (
  IN       UINT64               Ccsidr,
  IN       BOOLEAN              Ccidx,
  IN CONST SKY1_CACHE_GEOMETRY  *Expected
  );

/**
  Compute the size of the PPTT for a CPU topology.

  @param [in] CpuTopo         CPU topology from the Configuration Manager.
  @param [in] DescribeCaches  TRUE to include cache type structures.

  @return Table size in bytes, including the ACPI description header.
**/
UINT32
Sky1PpttSize (
  IN CONST CM_CIX_CPU_TOPO_INFO  *CpuTopo,
  IN       BOOLEAN               DescribeCaches
  );

/**
  Write the PPTT processor hierarchy and cache structures after the ACPI
  description header.

  Only enabled cores and clusters with at least one enabled core are
  described, keyed by their ACPI processor UIDs. With DescribeCaches, each
  core carries its L1 instruction and data caches (chained to its private L2
  if the core type has one) and the socket node carries the cache shared by
  all cores.

  @param [in]  CpuTopo         CPU topology from the Configuration Manager.
  @param [in]  DescribeCaches  TRUE to include cache type structures.
  @param [out] Table           Zeroed buffer of Sky1PpttSize () bytes.
**/
VOID
Sky1PpttWriteNodes (
  IN CONST CM_CIX_CPU_TOPO_INFO  *CpuTopo,
  IN       BOOLEAN               DescribeCaches,
  OUT      UINT8                 *Table
  );

#endif // PPTT_TOPOLOGY_H_
