#/** @file PciHostBridgeLib.c
#
#  Copyright 2024 Cix Technology Group Co., Ltd. All Rights Reserved.
#
#**/

#include <PiDxe.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/PciHostBridgeLib.h>
#include <Protocol/PciHostBridgeResourceAllocation.h>
#include <Protocol/PciRootBridgeIo.h>
#include <Library/PlatformPcieLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/ConfigParamsDataBlockLib.h>
#include <Library/PciSegmentLib.h>
#include <Protocol/ConfigParamsManageProtocol.h>
#include <Guid/DtPlatformFormSet.h>

#include "PcieWindow.h"

//
// Outbound address translation of the RB0 controller: the Cadence HPA
// AXI-slave register bank sits at 0x9000 in the controller's "reg" space,
// and outbound region r starts at 0x1000 + r * 0x80 within it. Read back on
// the live x4/x2 controllers, PcieInitDxe programs each 64-bit window as one
// region of exactly the stock 16 GiB.
//
#define RB0_ATU_OB_REGION(Region)  (PCI_HB0RB0_PCI_BASE + 0x9000 + 0x1000 + (Region) * 0x80)

STATIC
UINT32
PcieWindowCfgRead32 (
  IN VOID    *Context,
  IN UINT8   Bus,
  IN UINT8   Device,
  IN UINT8   Function,
  IN UINT16  Offset
  )
{
  return PciSegmentRead32 (PCI_SEGMENT_LIB_ADDRESS (0, Bus, Device, Function, Offset));
}

STATIC
VOID
PcieWindowCfgWrite32 (
  IN VOID    *Context,
  IN UINT8   Bus,
  IN UINT8   Device,
  IN UINT8   Function,
  IN UINT16  Offset,
  IN UINT32  Value
  )
{
  PciSegmentWrite32 (PCI_SEGMENT_LIB_ADDRESS (0, Bus, Device, Function, Offset), Value);
}

/**
  Make RB0's outbound translation cover a grown 64-bit window.

  @retval TRUE   The window is translated (no write was needed, or the
                 planned writes were made).
  @retval FALSE  The current programming is not the expected stock layout;
                 nothing was written.
**/
STATIC
BOOLEAN
CoverRb0Window (
  IN UINT64  Base,
  IN UINT64  Size
  )
{
  PCIE_ATU_OB_REGION  Regions[PCIE_ATU_OB_REGION_COUNT];
  PCIE_ATU_PLAN       Plan;
  UINTN               Index;
  UINTN               Address;

  for (Index = 0; Index < PCIE_ATU_OB_REGION_COUNT; Index++) {
    Address                  = RB0_ATU_OB_REGION (Index);
    Regions[Index].CpuAddr0  = MmioRead32 (Address + 0x00);
    Regions[Index].CpuAddr1  = MmioRead32 (Address + 0x04);
    Regions[Index].Desc0     = MmioRead32 (Address + 0x08);
    Regions[Index].Desc1     = MmioRead32 (Address + 0x0C);
    Regions[Index].PciAddr0  = MmioRead32 (Address + 0x10);
    Regions[Index].PciAddr1  = MmioRead32 (Address + 0x14);
    Regions[Index].Ctrl0     = MmioRead32 (Address + 0x18);
  }

  if (!PcieWindowPlanAtu (Regions, Base, PCIE_WINDOW_RB0_STOCK_SIZE, Size, &Plan)) {
    return FALSE;
  }

  //
  // Program the added region completely before it decodes (translation and
  // descriptors first, as Linux does for this IP), then widen the stock one.
  //
  if (Plan.AddIndex != PCIE_ATU_NO_REGION) {
    Address = RB0_ATU_OB_REGION (Plan.AddIndex);
    MmioWrite32 (Address + 0x10, Plan.Add.PciAddr0);
    MmioWrite32 (Address + 0x14, Plan.Add.PciAddr1);
    MmioWrite32 (Address + 0x08, Plan.Add.Desc0);
    MmioWrite32 (Address + 0x0C, Plan.Add.Desc1);
    MmioWrite32 (Address + 0x04, Plan.Add.CpuAddr1);
    MmioWrite32 (Address + 0x00, Plan.Add.CpuAddr0);
    MmioWrite32 (Address + 0x18, Plan.Add.Ctrl0);
  }

  if (Plan.WidenIndex != PCIE_ATU_NO_REGION) {
    MmioWrite32 (RB0_ATU_OB_REGION (Plan.WidenIndex), Plan.WidenCpuAddr0);
  }

  return TRUE;
}

/**
  TRUE when the OS gets ACPI tables rather than the device tree.

  The DT carries a static "ranges" for RB0 that no firmware path updates, so
  the window only grows when ACPI describes it. DtPlatformDxe keeps the
  choice in DtAcpiPref and falls back to PcdDefaultDtPref when it is unset;
  any other read failure counts as DT so that the window stays stock.
**/
STATIC
BOOLEAN
IsAcpiBoot (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT8       Pref[4];
  UINTN       Size;

  Size   = sizeof (Pref);
  Status = gRT->GetVariable (L"DtAcpiPref", &gDtPlatformFormSetGuid, NULL, &Size, Pref);
  if (Status == EFI_NOT_FOUND) {
    return (BOOLEAN)!PcdGetBool (PcdDefaultDtPref);
  }

  return (BOOLEAN)(!EFI_ERROR (Status) && (Size >= 1) && (Pref[0] == 1));
}

/**
  Size of RB0's 64-bit window, decided once per boot.

  The first caller (PciHostBridgeDxe, before PCI enumeration) resolves it
  from PcdPcieRb0Mem64Layout and, for Auto, from the resizable BARs below
  RB0, then publishes it in PcdPcieRb0Mem64Size; every later caller and the
  ACPI tables use that value. With the slot empty the stock layout is used
  and nothing is touched.
**/
STATIC
UINT64
GetRb0Mem64Size (
  IN CONST CONFIG_PARAMS_DATA_BLOCK  *ConfigData
  )
{
  CONST PCI_ROOT_BRIDGE_RESOURCE_APPETURE  *Rb0;
  PCIE_WINDOW_CFG                          Cfg;
  PCIE_WINDOW_LAYOUT                       Forced;
  PCIE_WINDOW_LAYOUT                       Layout;
  UINT64                                   MaxRebar;
  UINT64                                   Size;
  EFI_STATUS                               Status;

  Size = PcdGet64 (PcdPcieRb0Mem64Size);
  if (Size != 0) {
    return Size;
  }

  Rb0      = &mPcieResourceAppeture[0];
  Layout   = PcieWindowLayoutStock16G;
  MaxRebar = 0;

  if (ConfigData->Pcie.PcieLinkUpStatus[0] && IsAcpiBoot () &&
      (Rb0->MemAbove4G == PCIE_WINDOW_RB0_MEM64_BASE) &&
      (Rb0->MemAbove4GSize == PCIE_WINDOW_RB0_STOCK_SIZE) &&
      (Rb0->PMemAbove4G == PCIE_WINDOW_RB0_MEM64_BASE) &&
      (Rb0->PMemAbove4GSize == PCIE_WINDOW_RB0_STOCK_SIZE))
  {
    Forced = (PCIE_WINDOW_LAYOUT)FixedPcdGet8 (PcdPcieRb0Mem64Layout);
    if ((UINTN)Forced >= PcieWindowLayoutCount) {
      DEBUG ((DEBUG_ERROR, "%a: invalid PcdPcieRb0Mem64Layout %u, using Auto\n", __func__, Forced));
      Forced = PcieWindowLayoutAuto;
    }

    if (Forced == PcieWindowLayoutAuto) {
      Cfg.Read32  = PcieWindowCfgRead32;
      Cfg.Write32 = PcieWindowCfgWrite32;
      Cfg.Context = NULL;
      MaxRebar    = PcieWindowHierarchyMaxRebar (&Cfg, (UINT8)Rb0->BusBase, (UINT8)Rb0->BusLimit);
    }

    Layout = PcieWindowSelect (Forced, MaxRebar);

    //
    // A window the controller does not translate would be a lie to the OS;
    // fall back to smaller layouts until one is covered.
    //
    while ((Layout > PcieWindowLayoutStock16G) &&
           !CoverRb0Window (PCIE_WINDOW_RB0_MEM64_BASE, PcieWindowLayoutSize (Layout)))
    {
      DEBUG ((DEBUG_ERROR, "%a: RB0 translation cannot cover layout %u\n", __func__, Layout));
      Layout--;
    }
  }

  Size = PcieWindowLayoutSize (Layout);
  DEBUG ((
    DEBUG_INFO,
    "%a: largest resizable BAR 0x%lx, RB0 64-bit window 0x%lx..0x%lx\n",
    __func__,
    MaxRebar,
    PCIE_WINDOW_RB0_MEM64_BASE,
    PCIE_WINDOW_RB0_MEM64_BASE + Size - 1
    ));

  Status = PcdSet64S (PcdPcieRb0Mem64Size, Size);
  ASSERT_EFI_ERROR (Status);

  return Size;
}

#define EFI_PCI_SUPPORT  (EFI_PCI_ATTRIBUTE_ISA_MOTHERBOARD_IO |  \
                           EFI_PCI_ATTRIBUTE_IDE_SECONDARY_IO | \
                           EFI_PCI_ATTRIBUTE_IDE_PRIMARY_IO | \
                           EFI_PCI_ATTRIBUTE_ISA_IO_16  | \
                           EFI_PCI_ATTRIBUTE_VGA_MEMORY | \
                           EFI_PCI_ATTRIBUTE_VGA_IO_16  | \
                           EFI_PCI_ATTRIBUTE_VGA_PALETTE_IO_16)

#define EFI_PCI_ATTRIBUTE  EFI_PCI_SUPPORT

#pragma pack(1)
typedef struct {
  ACPI_HID_DEVICE_PATH        AcpiDevicePath;
  EFI_DEVICE_PATH_PROTOCOL    EndDevicePath;
} EFI_PCI_ROOT_BRIDGE_DEVICE_PATH;
#pragma pack ()

STATIC EFI_PCI_ROOT_BRIDGE_DEVICE_PATH  mEfiPciRootBridgeDevicePath = {
  {
    {
      ACPI_DEVICE_PATH,
      ACPI_DP,
      {
        (UINT8)(sizeof (ACPI_HID_DEVICE_PATH)),
        (UINT8)((sizeof (ACPI_HID_DEVICE_PATH)) >> 8)
      }
    },
    EISA_PNP_ID (0x0A03), // PCI
    0
  },  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    {
      END_DEVICE_PATH_LENGTH,
      0
    }
  }
};

STATIC PCI_ROOT_BRIDGE  mRootBridgeTemplate = {
  0,                                // Segment
  EFI_PCI_SUPPORT,                  // Supports
  EFI_PCI_ATTRIBUTE,                // Attributes
  TRUE,                             // DmaAbove4G
  FALSE,                            // NoExtendedConfigSpace
  FALSE,                            // ResourceAssigned
  EFI_PCI_HOST_BRIDGE_MEM64_DECODE, // AllocationAttributes
  {
    // Bus
    0,
    0
  },{
    // Io
    0,
    0,
    0
  },{
    // Mem
    MAX_UINT64,
    0,
    0
  },{
    // MemAbove4G
    MAX_UINT64,
    0,
    0
  },{
    // PMem
    MAX_UINT64,
    0,
    0
  },{
    // PMemAbove4G
    MAX_UINT64,
    0,
    0
  },
  (EFI_DEVICE_PATH_PROTOCOL *)&mEfiPciRootBridgeDevicePath
};

EFI_STATUS
ConstructRootBridge (
  PCI_ROOT_BRIDGE                    *Bridge,
  PCI_ROOT_BRIDGE_RESOURCE_APPETURE  *Appeture,
  UINT8                              RootPortIndex
  )
{
  EFI_PCI_ROOT_BRIDGE_DEVICE_PATH  *DevicePath;

  DEBUG ((DEBUG_ERROR, "Construct Root Bridge resource\n"));

  CopyMem (Bridge, &mRootBridgeTemplate, sizeof *Bridge);
  Bridge->Segment   = Appeture->Segment;
  Bridge->Bus.Base  = Appeture->BusBase;
  Bridge->Bus.Limit = Appeture->BusLimit;

  if (Appeture->IoBase) {
    Bridge->Io.Base  = Appeture->IoBase;
    Bridge->Io.Limit = Appeture->IoBase + Appeture->IoSize - 1;
  } else {
    Bridge->Io.Base  = 0;
    Bridge->Io.Limit = 0xFFFF;
  }

  if (Appeture->Mem) {
    Bridge->Mem.Base  = Appeture->Mem;
    Bridge->Mem.Limit = Appeture->Mem + Appeture->MemSize - 1;
  } else {
    Bridge->Mem.Base  = MAX_UINT64;
    Bridge->Mem.Limit = 0;
  }

  if (Appeture->MemAbove4G) {
    Bridge->MemAbove4G.Base  = Appeture->MemAbove4G;
    Bridge->MemAbove4G.Limit = Appeture->MemAbove4G + Appeture->MemAbove4GSize - 1;
  } else {
    Bridge->MemAbove4G.Base  = MAX_UINT64;
    Bridge->MemAbove4G.Limit = 0;
  }

  if (Appeture->PMem) {
    Bridge->PMem.Base  = Appeture->PMem;
    Bridge->PMem.Limit = Appeture->PMem + Appeture->PMemSize - 1;
  } else {
    Bridge->PMem.Base  = MAX_UINT64;
    Bridge->PMem.Limit = 0;
  }

  if (Appeture->PMemAbove4G) {
    Bridge->PMemAbove4G.Base  = Appeture->PMemAbove4G;
    Bridge->PMemAbove4G.Limit = Appeture->PMemAbove4G + Appeture->PMemAbove4GSize -1;
  } else {
    Bridge->PMemAbove4G.Base  = MAX_UINT64;
    Bridge->PMemAbove4G.Limit = 0;
  }

  DevicePath = AllocateCopyPool (sizeof mEfiPciRootBridgeDevicePath, &mEfiPciRootBridgeDevicePath);
  if (DevicePath == NULL) {
    DEBUG ((DEBUG_ERROR, "[%a]:[%dL] AllocatePool failed!\n", __FUNCTION__, __LINE__));
    return EFI_OUT_OF_RESOURCES;
  }

  DevicePath->AcpiDevicePath.UID = RootPortIndex;// Root Port Number
  Bridge->DevicePath             = (EFI_DEVICE_PATH_PROTOCOL *)DevicePath;
  return EFI_SUCCESS;
}

/**
  Return all the root bridge instances in an array.

  @param Count  Return the count of root bridge instances.

  @return All the root bridge instances in an array.

**/
PCI_ROOT_BRIDGE *
EFIAPI
PciHostBridgeGetRootBridges (
  OUT UINTN  *Count
  )
{
  EFI_STATUS                         Status;
  UINTN                              Loop;
  UINTN                              RootBridgeCount = 0;
  PCI_ROOT_BRIDGE                    *Bridges;
  CONFIG_PARAMS_DATA_BLOCK           *ConfigData = NULL;
  CIX_CONFIG_PARAMS_MANAGE_PROTOCOL  *ConfigManage;
  PCI_ROOT_BRIDGE_RESOURCE_APPETURE  Appeture;
  UINT64                             Rb0Mem64Size;

  // Set default value to 0 in case we got any error.
  *Count = 0;

  RootBridgeCount = PCIE_MAX_ROOTBRIDGE;

  Bridges = AllocatePool (RootBridgeCount * sizeof *Bridges);
  if (Bridges == NULL) {
    DEBUG ((DEBUG_ERROR, "[%a:%d] - AllocatePool failed!\n", __FUNCTION__, __LINE__));
    return NULL;
  }

  Status = gBS->LocateProtocol (&gCixConfigParamsManageProtocolGuid, NULL, (VOID **)&ConfigManage);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Could not locate CIX Config Params Manage Protocol\n"));
    FreePool (Bridges);
    return NULL;
  }

  ConfigData = ConfigManage->Data;

  if (ConfigData == NULL) {
    DEBUG ((EFI_D_ERROR, "[%a]ConfigData is NULL\n", __FUNCTION__));
    FreePool (Bridges);
    return NULL;
  }

  Rb0Mem64Size = GetRb0Mem64Size (ConfigData);

  for (Loop = 0; Loop < PCIE_MAX_ROOTBRIDGE; Loop++) {
    if (ConfigData->Pcie.PcieLinkUpStatus[Loop] == FALSE) {
      continue;
    }

    CopyMem (&Appeture, &mPcieResourceAppeture[Loop], sizeof (Appeture));
    if ((Loop == 0) &&
        (Appeture.MemAbove4G == PCIE_WINDOW_RB0_MEM64_BASE) &&
        (Appeture.PMemAbove4G == PCIE_WINDOW_RB0_MEM64_BASE))
    {
      Appeture.MemAbove4GSize  = Rb0Mem64Size;
      Appeture.PMemAbove4GSize = Rb0Mem64Size;
    }

    Status = ConstructRootBridge (&Bridges[*Count], &Appeture, Loop);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[%a:%d] - ConstructRootBridge failed!\n", __FUNCTION__, __LINE__));
      continue;
    }

    (*Count)++;
  }

  if (*Count == 0) {
    FreePool (Bridges);
    return NULL;
  }

  return Bridges;
}

/**
  Free the root bridge instances array returned from PciHostBridgeGetRootBridges().

  @param Bridges The root bridge instances array.
  @param Count   The count of the array.
**/
VOID
EFIAPI
PciHostBridgeFreeRootBridges (
  PCI_ROOT_BRIDGE  *Bridges,
  UINTN            Count
  )
{
}

GLOBAL_REMOVE_IF_UNREFERENCED CHAR16  *mPciHostBridgeLibAcpiAddressSpaceTypeStr[] = {
  L"Mem", L"I/O", L"Bus"
};

/**
  Inform the platform that the resource conflict happens.

  @param HostBridgeHandle Handle of the Host Bridge.
  @param Configuration    Pointer to PCI I/O and PCI memory resource
                          descriptors. The Configuration contains the resources
                          for all the root bridges. The resource for each root
                          bridge is terminated with END descriptor and an
                          additional END is appended indicating the end of the
                          entire resources. The resource descriptor field
                          values follow the description in
                          EFI_PCI_HOST_BRIDGE_RESOURCE_ALLOCATION_PROTOCOL
                          .SubmitResources().

**/
VOID
EFIAPI
PciHostBridgeResourceConflict (
  EFI_HANDLE  HostBridgeHandle,
  VOID        *Configuration
  )
{
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR  *Descriptor;
  UINTN                              RootBridgeIndex;

  DEBUG ((DEBUG_ERROR, "PciHostBridge: Resource conflict happens!\n"));

  RootBridgeIndex = 0;
  Descriptor      = (EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *)Configuration;

  while (Descriptor->Desc == ACPI_ADDRESS_SPACE_DESCRIPTOR) {
    DEBUG ((DEBUG_ERROR, "RootBridge[%d]:\n", RootBridgeIndex++));

    for ( ; Descriptor->Desc == ACPI_ADDRESS_SPACE_DESCRIPTOR; Descriptor++) {
      ASSERT (Descriptor->ResType < sizeof (mPciHostBridgeLibAcpiAddressSpaceTypeStr) / sizeof (mPciHostBridgeLibAcpiAddressSpaceTypeStr[0]));

      DEBUG (
        (
         DEBUG_ERROR,
         " %s: Length/Alignment = 0x%lx / 0x%lx\n",
         mPciHostBridgeLibAcpiAddressSpaceTypeStr[Descriptor->ResType],
         Descriptor->AddrLen,
         Descriptor->AddrRangeMax
        )
        );

      if (Descriptor->ResType == ACPI_ADDRESS_SPACE_TYPE_MEM) {
        DEBUG (
          (DEBUG_ERROR, "     Granularity/SpecificFlag = %ld / %02x%s\n",
           Descriptor->AddrSpaceGranularity, Descriptor->SpecificFlag,
           ((Descriptor->SpecificFlag &
             EFI_ACPI_MEMORY_RESOURCE_SPECIFIC_FLAG_CACHEABLE_PREFETCHABLE
             ) != 0) ? L" (Prefetchable)" : L""
          )
          );
      }
    }

    //
    // Skip the END descriptor for root bridge
    //
    ASSERT (Descriptor->Desc == ACPI_END_TAG_DESCRIPTOR);
    Descriptor = (EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *)(
                                                       (EFI_ACPI_END_TAG_DESCRIPTOR *)Descriptor + 1
                                                       );
  }

  return;
}
