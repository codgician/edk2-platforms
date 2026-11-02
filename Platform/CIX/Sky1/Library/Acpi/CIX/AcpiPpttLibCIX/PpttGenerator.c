/** @file
  PPTT Table Generator

  Copyright 2024 Cix Technology Group Co., Ltd. All Rights Reserved.
  Copyright (c) 2021, ARM Limited. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent

  @par Reference(s):
  - ACPI 6.4 Specification, January 2021

  @par Glossary:
  - Cm or CM   - Configuration Manager
  - Obj or OBJ - Object
**/

#include <Library/AcpiLib.h>
#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Protocol/AcpiTable.h>

// Module specific include files.
#include <AcpiTableGenerator.h>
#include <ConfigurationManagerObject.h>
#include <ConfigurationManagerHelper.h>
#include <AcpiNameSpaceObjects.h>
#include <Library/TableHelperLib.h>
#include <Protocol/ConfigurationManagerProtocol.h>

#include "PpttTopology.h"

/**
  This macro expands to a function that retrieves the Cpu
  topology information
*/
GET_OBJECT_LIST (
  EObjNameSpaceOem,
  ECixObjCpuTopoInfo,
  CM_CIX_CPU_TOPO_INFO
  );

#define CLIDR_CTYPE_MASK     0x1FFFFF
#define CSSELR_LEVEL(Level)  (((Level) - 1) << 1)
#define CSSELR_IND           BIT0

/**
  Check that the boot core reports the cache geometry the PPTT publishes.

  The geometry in PpttTopology.c is that of Sky1. Reading the boot core's
  CLIDR_EL1 and CCSIDR_EL1 keeps it from being published on a part whose
  caches differ; such a part gets a PPTT without cache structures.

  @retval TRUE   The boot core's caches match the published geometry.
  @retval FALSE  Unknown core or different geometry.
**/
STATIC
BOOLEAN
BootCoreMatchesCacheGeometry (
  VOID
  )
{
  CONST SKY1_CORE_CACHES  *Caches;
  BOOLEAN                 Ccidx;
  UINT32                  SharedCsselr;

  Caches = Sky1CoreCachesForMidr (ArmReadMidr ());
  if (Caches == NULL) {
    DEBUG ((DEBUG_ERROR, "PPTT: boot core MIDR 0x%lx is not a Sky1 core, caches not described\n", ArmReadMidr ()));
    return FALSE;
  }

  if ((ReadCLIDR () & CLIDR_CTYPE_MASK) != Caches->ClidrCtype) {
    DEBUG ((DEBUG_ERROR, "PPTT: boot core CLIDR 0x%x unexpected, caches not described\n", ReadCLIDR ()));
    return FALSE;
  }

  Ccidx        = ArmHasCcidx ();
  SharedCsselr = (Caches->L2.Size != 0) ? CSSELR_LEVEL (3) : CSSELR_LEVEL (2);
  if (!Sky1CcsidrMatches (ReadCCSIDR (CSSELR_LEVEL (1)), Ccidx, &Caches->L1) ||
      !Sky1CcsidrMatches (ReadCCSIDR (CSSELR_LEVEL (1) | CSSELR_IND), Ccidx, &Caches->L1) ||
      ((Caches->L2.Size != 0) && !Sky1CcsidrMatches (ReadCCSIDR (CSSELR_LEVEL (2)), Ccidx, &Caches->L2)) ||
      !Sky1CcsidrMatches (ReadCCSIDR (SharedCsselr), Ccidx, &gSky1SharedCache))
  {
    DEBUG ((DEBUG_ERROR, "PPTT: boot core cache geometry unexpected, caches not described\n"));
    return FALSE;
  }

  return TRUE;
}

/**
  Construct the PPTT ACPI table.

  This function invokes the Configuration Manager protocol interface
  to get the required hardware information for generating the ACPI
  table.

  If this function allocates any resources then they must be freed
  in the FreeXXXXTableResources function.

  @param [in]  This                 Pointer to the table generator.
  @param [in]  AcpiTableInfo        Pointer to the ACPI table generator to be used.
  @param [in]  CfgMgrProtocol       Pointer to the Configuration Manager
                                    Protocol Interface.
  @param [out] Table                Pointer to the constructed ACPI Table.

  @retval EFI_SUCCESS               Table generated successfully.
  @retval EFI_INVALID_PARAMETER     A parameter is invalid.
  @retval EFI_NOT_FOUND             The required object was not found.
  @retval EFI_BAD_BUFFER_SIZE       The size returned by the Configuration
                                    Manager is less than the Object size for
                                    the requested object.
**/
STATIC
EFI_STATUS
EFIAPI
BuildPpttTable (
  IN  CONST ACPI_TABLE_GENERATOR                  *CONST  This,
  IN  CONST CM_STD_OBJ_ACPI_TABLE_INFO            *CONST  AcpiTableInfo,
  IN  CONST EDKII_CONFIGURATION_MANAGER_PROTOCOL  *CONST  CfgMgrProtocol,
  OUT       EFI_ACPI_DESCRIPTION_HEADER          **CONST  Table
  )
{
  EFI_STATUS            Status;
  UINT32                TableSize;
  CM_CIX_CPU_TOPO_INFO  *CpuTopoInfo;
  UINT32                CpuTopoInfoCount;
  BOOLEAN               DescribeCaches;

  EFI_ACPI_6_3_PROCESSOR_PROPERTIES_TOPOLOGY_TABLE_HEADER  *Pptt;

  ASSERT (
    (This != NULL) &&
    (AcpiTableInfo != NULL) &&
    (CfgMgrProtocol != NULL) &&
    (Table != NULL) &&
    (AcpiTableInfo->TableGeneratorId == This->GeneratorID) &&
    (AcpiTableInfo->AcpiTableSignature == This->AcpiTableSignature)
    );

  if ((AcpiTableInfo->AcpiTableRevision < This->MinAcpiTableRevision) ||
      (AcpiTableInfo->AcpiTableRevision > This->AcpiTableRevision))
  {
    DEBUG (
      (
       DEBUG_ERROR,
       "ERROR: PPTT: Requested table revision = %d is not supported. "
       "Supported table revisions: Minimum = %d. Maximum = %d\n",
       AcpiTableInfo->AcpiTableRevision,
       This->MinAcpiTableRevision,
       This->AcpiTableRevision
      )
      );
    return EFI_INVALID_PARAMETER;
  }

  *Table = NULL;

  Status = GetECixObjCpuTopoInfo (
             CfgMgrProtocol,
             CM_NULL_TOKEN,
             &CpuTopoInfo,
             &CpuTopoInfoCount
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "ERROR: PPTT: Failed to get processor topology info. Status = %r\n", Status));
    goto error_handler;
  }

  DescribeCaches = BootCoreMatchesCacheGeometry ();
  TableSize      = Sky1PpttSize (CpuTopoInfo, DescribeCaches);
  DEBUG ((DEBUG_INFO, "INFO: PPTT: TableSize = %d, caches %a\n", TableSize, DescribeCaches ? "described" : "omitted"));

  // Allocate the Buffer for the PPTT table
  *Table = (EFI_ACPI_DESCRIPTION_HEADER *)AllocateZeroPool (TableSize);
  if (*Table == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    DEBUG (
      (
       DEBUG_ERROR,
       "ERROR: PPTT: Failed to allocate memory for PPTT Table. " \
       "Size = %d. Status = %r\n",
       TableSize,
       Status
      )
      );
    goto error_handler;
  }

  Pptt = (EFI_ACPI_6_3_PROCESSOR_PROPERTIES_TOPOLOGY_TABLE_HEADER *)*Table;

  // Add ACPI header
  Status = AddAcpiHeader (
             CfgMgrProtocol,
             This,
             &Pptt->Header,
             AcpiTableInfo,
             TableSize
             );
  if (EFI_ERROR (Status)) {
    DEBUG (
      (
       DEBUG_ERROR,
       "ERROR: PPTT: Failed to add ACPI header. Status = %r\n",
       Status
      )
      );
    goto error_handler;
  }

  Sky1PpttWriteNodes (CpuTopoInfo, DescribeCaches, (UINT8 *)Pptt);

  return Status;

error_handler:

  if (*Table != NULL) {
    FreePool (*Table);
    *Table = NULL;
  }

  return Status;
}

/**
  Free any resources allocated for constructing the PPTT

  @param [in]      This             Pointer to the table generator.
  @param [in]      AcpiTableInfo    Pointer to the ACPI Table Info.
  @param [in]      CfgMgrProtocol   Pointer to the Configuration Manager
                                    Protocol Interface.
  @param [in, out] Table            Pointer to the ACPI Table.

  @retval EFI_SUCCESS               The resources were freed successfully.
  @retval EFI_INVALID_PARAMETER     The table pointer is NULL or invalid.
**/
STATIC
EFI_STATUS
FreePpttTableResources (
  IN      CONST ACPI_TABLE_GENERATOR                  *CONST  This,
  IN      CONST CM_STD_OBJ_ACPI_TABLE_INFO            *CONST  AcpiTableInfo,
  IN      CONST EDKII_CONFIGURATION_MANAGER_PROTOCOL  *CONST  CfgMgrProtocol,
  IN OUT        EFI_ACPI_DESCRIPTION_HEADER          **CONST  Table
  )
{
  ASSERT (
    (This != NULL) &&
    (AcpiTableInfo != NULL) &&
    (CfgMgrProtocol != NULL) &&
    (AcpiTableInfo->TableGeneratorId == This->GeneratorID) &&
    (AcpiTableInfo->AcpiTableSignature == This->AcpiTableSignature)
    );

  if ((Table == NULL) || (*Table == NULL)) {
    DEBUG ((DEBUG_ERROR, "ERROR: PPTT: Invalid Table Pointer\n"));
    ASSERT (
      (Table != NULL) &&
      (*Table != NULL)
      );
    return EFI_INVALID_PARAMETER;
  }

  FreePool (*Table);
  *Table = NULL;
  return EFI_SUCCESS;
}

/** The interface for the PPTT Table Generator.
*/
STATIC
ACPI_TABLE_GENERATOR  PpttGenerator = {
  // Generator ID
  CREATE_STD_ACPI_TABLE_GEN_ID (EStdAcpiTableIdPptt),
  // Generator Description
  L"ACPI.STD.PPTT.GENERATOR",
  // ACPI Table Signature
  EFI_ACPI_6_3_PROCESSOR_PROPERTIES_TOPOLOGY_TABLE_STRUCTURE_SIGNATURE,
  // ACPI Table Revision supported by this Generator
  EFI_ACPI_6_3_PROCESSOR_PROPERTIES_TOPOLOGY_TABLE_REVISION,
  // Minimum supported ACPI Table Revision
  EFI_ACPI_6_3_PROCESSOR_PROPERTIES_TOPOLOGY_TABLE_REVISION,
  // Creator ID
  EFI_ACPI_CREATOR_ID,
  // Creator Revision
  EFI_ACPI_CREATOR_REVISION,
  // Build Table function
  BuildPpttTable,
  // Free Resource function
  FreePpttTableResources,
  // Extended build function not needed
  NULL,
  // Extended build function not implemented by the generator.
  // Hence extended free resource function is not required.
  NULL
};

/**
  Register the Generator with the ACPI Table Factory.

  @param [in]  ImageHandle        The handle to the image.
  @param [in]  SystemTable        Pointer to the System Table.

  @retval EFI_SUCCESS             The Generator is registered.
  @retval EFI_INVALID_PARAMETER   A parameter is invalid.
  @retval EFI_ALREADY_STARTED     The Generator for the Table ID
                                  is already registered.
**/
EFI_STATUS
EFIAPI
AcpiPpttLibConstructor (
  IN  EFI_HANDLE        ImageHandle,
  IN  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  Status = RegisterAcpiTableGenerator (&PpttGenerator);
  DEBUG ((DEBUG_INFO, "PPTT: Register Generator. Status = %r\n", Status));
  ASSERT_EFI_ERROR (Status);
  return Status;
}

/**
  Deregister the Generator from the ACPI Table Factory.

  @param [in]  ImageHandle        The handle to the image.
  @param [in]  SystemTable        Pointer to the System Table.

  @retval EFI_SUCCESS             The Generator is deregistered.
  @retval EFI_INVALID_PARAMETER   A parameter is invalid.
  @retval EFI_NOT_FOUND           The Generator is not registered.
**/
EFI_STATUS
EFIAPI
AcpiPpttLibDestructor (
  IN  EFI_HANDLE        ImageHandle,
  IN  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  Status = DeregisterAcpiTableGenerator (&PpttGenerator);
  DEBUG ((DEBUG_INFO, "PPTT: Deregister Generator. Status = %r\n", Status));
  ASSERT_EFI_ERROR (Status);
  return Status;
}
