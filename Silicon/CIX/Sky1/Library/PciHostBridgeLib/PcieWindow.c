/** @file
  Sizing of the 64-bit MMIO window of the Sky1 x8 root bridge (RB0).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <IndustryStandard/Pci.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include "PcieWindow.h"

#define PCIE_EXT_CAP_START       0x100
#define PCIE_EXT_CAP_END         0x1000
#define PCIE_EXT_CAP_MAX_HOPS    ((PCIE_EXT_CAP_END - PCIE_EXT_CAP_START) / 4)
#define PCI_CAP_MAX_HOPS         48
#define PCIE_REBAR_ID            0x0015
#define PCIE_REBAR_MAX_BARS      6
#define PCIE_PORT_TYPE_UPSTREAM  0x5
#define PCIE_WINDOW_MAX_DEPTH    8
#define PCIE_WINDOW_MAX_BRIDGES  64

#define ATU_NBITS(Addr0)    (((Addr0) & 0x3F) + 1)
#define ATU_BASE(Lo, Hi)    (LShiftU64 ((Hi), 32) | ((Lo) & 0xFFFFFF00))
#define ATU_DESC0_TYPE(D0)  (((D0) >> 24) & 0x1F)
#define ATU_TYPE_MEM        0x0
#define ATU_PCI_IDENTITY    0x3F

typedef struct {
  UINT8     Bus;
  UINT8     Device;
  UINT8     Function;
  UINT32    BusNumbers;
} SAVED_BRIDGE;

typedef struct {
  CONST PCIE_WINDOW_CFG    *Cfg;
  UINT8                    NextBus;
  UINT8                    BusLimit;
  BOOLEAN                  OutOfBuses;
  UINTN                    SavedCount;
  SAVED_BRIDGE             Saved[PCIE_WINDOW_MAX_BRIDGES];
} SCAN_STATE;

STATIC CONST UINT64  mLayoutSize[PcieWindowLayoutCount] = {
  0,
  SIZE_16GB,
  SIZE_32GB,
  SIZE_64GB
};

UINT64
PcieWindowLayoutSize (
  IN PCIE_WINDOW_LAYOUT  Layout
  )
{
  if ((UINTN)Layout >= PcieWindowLayoutCount) {
    return 0;
  }

  return mLayoutSize[Layout];
}

STATIC
UINT32
CfgRead (
  IN CONST PCIE_WINDOW_CFG  *Cfg,
  IN UINT8                  Bus,
  IN UINT8                  Device,
  IN UINT8                  Function,
  IN UINT16                 Offset
  )
{
  return Cfg->Read32 (Cfg->Context, Bus, Device, Function, Offset);
}

STATIC
BOOLEAN
FunctionPresent (
  IN CONST PCIE_WINDOW_CFG  *Cfg,
  IN UINT8                  Bus,
  IN UINT8                  Device,
  IN UINT8                  Function
  )
{
  UINT16  VendorId;

  VendorId = (UINT16)CfgRead (Cfg, Bus, Device, Function, PCI_VENDOR_ID_OFFSET);
  return (BOOLEAN)((VendorId != 0xFFFF) && (VendorId != 0x0000));
}

UINT16
PcieWindowFindExtCapability (
  IN CONST PCIE_WINDOW_CFG  *Cfg,
  IN UINT8                  Bus,
  IN UINT8                  Device,
  IN UINT8                  Function,
  IN UINT16                 CapabilityId
  )
{
  UINT32  Offset;
  UINT32  Header;
  UINTN   Hop;

  Offset = PCIE_EXT_CAP_START;
  for (Hop = 0; Hop < PCIE_EXT_CAP_MAX_HOPS; Hop++) {
    if ((Offset < PCIE_EXT_CAP_START) || (Offset >= PCIE_EXT_CAP_END)) {
      return 0;
    }

    Header = CfgRead (Cfg, Bus, Device, Function, (UINT16)Offset);
    if ((Header == 0) || (Header == MAX_UINT32)) {
      return 0;
    }

    if ((Header & 0xFFFF) == CapabilityId) {
      return (UINT16)Offset;
    }

    //
    // The two low bits of the next pointer are reserved.
    //
    Offset = (Header >> 20) & 0xFFC;
    if (Offset == 0) {
      return 0;
    }
  }

  return 0;
}

STATIC
UINT8
FindCapability (
  IN CONST PCIE_WINDOW_CFG  *Cfg,
  IN UINT8                  Bus,
  IN UINT8                  Device,
  IN UINT8                  Function,
  IN UINT8                  CapabilityId
  )
{
  UINT32  Header;
  UINT8   Offset;
  UINTN   Hop;

  if ((CfgRead (Cfg, Bus, Device, Function, PCI_COMMAND_OFFSET) & (EFI_PCI_STATUS_CAPABILITY << 16)) == 0) {
    return 0;
  }

  Offset = (UINT8)(CfgRead (Cfg, Bus, Device, Function, PCI_CAPBILITY_POINTER_OFFSET) & 0xFC);
  for (Hop = 0; Hop < PCI_CAP_MAX_HOPS; Hop++) {
    if (Offset < 0x40) {
      return 0;
    }

    Header = CfgRead (Cfg, Bus, Device, Function, Offset);
    if ((Header & 0xFF) == 0xFF) {
      return 0;
    }

    if ((Header & 0xFF) == CapabilityId) {
      return Offset;
    }

    Offset = (UINT8)((Header >> 8) & 0xFC);
  }

  return 0;
}

UINT64
PcieWindowFunctionMaxRebar (
  IN CONST PCIE_WINDOW_CFG  *Cfg,
  IN UINT8                  Bus,
  IN UINT8                  Device,
  IN UINT8                  Function
  )
{
  UINT16  Offset;
  UINT32  BarCount;
  UINT32  Index;
  UINT32  Capability;
  UINT32  Control;
  UINT32  Bit;
  UINT64  Largest;

  Offset = PcieWindowFindExtCapability (Cfg, Bus, Device, Function, PCIE_REBAR_ID);
  if (Offset == 0) {
    return 0;
  }

  //
  // The number of resizable BARs lives in the first control register only.
  //
  BarCount = (CfgRead (Cfg, Bus, Device, Function, Offset + 8) >> 5) & 0x7;
  if ((BarCount == 0) || (BarCount > PCIE_REBAR_MAX_BARS) ||
      ((UINT32)Offset + 8 * BarCount + 4 > PCIE_EXT_CAP_END))
  {
    return 0;
  }

  Largest = 0;
  for (Index = 0; Index < BarCount; Index++) {
    Capability = CfgRead (Cfg, Bus, Device, Function, (UINT16)(Offset + 4 + 8 * Index));
    Control    = CfgRead (Cfg, Bus, Device, Function, (UINT16)(Offset + 8 + 8 * Index));

    //
    // Capability bits 4..31 advertise 1 MiB (2^20) .. 128 TiB (2^47);
    // control bits 16..31 continue with 256 TiB (2^48) .. 8 EiB (2^63).
    //
    for (Bit = 4; Bit < 32; Bit++) {
      if ((Capability & (1U << Bit)) != 0) {
        Largest = MAX (Largest, LShiftU64 (1, Bit + 16));
      }
    }

    for (Bit = 16; Bit < 32; Bit++) {
      if ((Control & (1U << Bit)) != 0) {
        Largest = MAX (Largest, LShiftU64 (1, Bit + 32));
      }
    }
  }

  return Largest;
}

STATIC
UINT64
ScanBus (
  IN OUT SCAN_STATE  *State,
  IN     UINT8       Bus,
  IN     BOOLEAN     OnlyDeviceZero,
  IN     UINTN       Depth
  );

/**
  Give the bridge at Bus:Device.Function the next free bus number, scan
  below it, and narrow its subordinate number to what the walk used so that
  later siblings do not overlap it.
**/
STATIC
UINT64
ScanBridge (
  IN OUT SCAN_STATE  *State,
  IN     UINT8       Bus,
  IN     UINT8       Device,
  IN     UINT8       Function,
  IN     UINTN       Depth
  )
{
  CONST PCIE_WINDOW_CFG  *Cfg;
  UINT32                 Original;
  UINT8                  Secondary;
  UINT8                  PcieCap;
  BOOLEAN                OnlyDeviceZero;
  UINT64                 Largest;

  Cfg = State->Cfg;
  if ((Depth >= PCIE_WINDOW_MAX_DEPTH) || State->OutOfBuses ||
      (State->SavedCount >= PCIE_WINDOW_MAX_BRIDGES))
  {
    return 0;
  }

  Secondary = State->NextBus;
  if (Secondary == State->BusLimit) {
    State->OutOfBuses = TRUE;
  } else {
    State->NextBus++;
  }

  Original                                     = CfgRead (Cfg, Bus, Device, Function, PCI_BRIDGE_PRIMARY_BUS_REGISTER_OFFSET);
  State->Saved[State->SavedCount].Bus          = Bus;
  State->Saved[State->SavedCount].Device       = Device;
  State->Saved[State->SavedCount].Function     = Function;
  State->Saved[State->SavedCount].BusNumbers   = Original;
  State->SavedCount++;

  Cfg->Write32 (
         Cfg->Context,
         Bus,
         Device,
         Function,
         PCI_BRIDGE_PRIMARY_BUS_REGISTER_OFFSET,
         (Original & 0xFF000000) | ((UINT32)State->BusLimit << 16) | ((UINT32)Secondary << 8) | Bus
         );

  //
  // A link below a root or downstream port carries a single device; only a
  // switch's internal bus behind its upstream port holds several.
  //
  OnlyDeviceZero = TRUE;
  PcieCap        = FindCapability (Cfg, Bus, Device, Function, EFI_PCI_CAPABILITY_ID_PCIEXP);
  if ((PcieCap != 0) &&
      (((CfgRead (Cfg, Bus, Device, Function, PcieCap) >> 20) & 0xF) == PCIE_PORT_TYPE_UPSTREAM))
  {
    OnlyDeviceZero = FALSE;
  }

  Largest = ScanBus (State, Secondary, OnlyDeviceZero, Depth + 1);

  Cfg->Write32 (
         Cfg->Context,
         Bus,
         Device,
         Function,
         PCI_BRIDGE_PRIMARY_BUS_REGISTER_OFFSET,
         (Original & 0xFF000000) |
         ((UINT32)(State->OutOfBuses ? State->BusLimit : (UINT8)(State->NextBus - 1)) << 16) |
         ((UINT32)Secondary << 8) | Bus
         );

  return Largest;
}

STATIC
UINT64
ScanBus (
  IN OUT SCAN_STATE  *State,
  IN     UINT8       Bus,
  IN     BOOLEAN     OnlyDeviceZero,
  IN     UINTN       Depth
  )
{
  CONST PCIE_WINDOW_CFG  *Cfg;
  UINT8                  Device;
  UINT8                  Function;
  UINT8                  FunctionCount;
  UINT8                  HeaderType;
  UINT64                 Largest;

  Cfg     = State->Cfg;
  Largest = 0;

  for (Device = 0; Device <= (OnlyDeviceZero ? 0 : PCI_MAX_DEVICE); Device++) {
    if (!FunctionPresent (Cfg, Bus, Device, 0)) {
      continue;
    }

    HeaderType    = (UINT8)(CfgRead (Cfg, Bus, Device, 0, PCI_CACHELINE_SIZE_OFFSET) >> 16);
    FunctionCount = ((HeaderType & HEADER_TYPE_MULTI_FUNCTION) != 0) ? PCI_MAX_FUNC + 1 : 1;

    for (Function = 0; Function < FunctionCount; Function++) {
      if (!FunctionPresent (Cfg, Bus, Device, Function)) {
        continue;
      }

      Largest    = MAX (Largest, PcieWindowFunctionMaxRebar (Cfg, Bus, Device, Function));
      HeaderType = (UINT8)(CfgRead (Cfg, Bus, Device, Function, PCI_CACHELINE_SIZE_OFFSET) >> 16);
      if ((HeaderType & HEADER_LAYOUT_CODE) == HEADER_TYPE_PCI_TO_PCI_BRIDGE) {
        Largest = MAX (Largest, ScanBridge (State, Bus, Device, Function, Depth));
      }
    }
  }

  return Largest;
}

UINT64
PcieWindowHierarchyMaxRebar (
  IN CONST PCIE_WINDOW_CFG  *Cfg,
  IN UINT8                  RootBus,
  IN UINT8                  BusLimit
  )
{
  SCAN_STATE  State;
  UINT64      Largest;
  UINT8       HeaderType;

  if ((BusLimit <= RootBus) || !FunctionPresent (Cfg, RootBus, 0, 0)) {
    return 0;
  }

  HeaderType = (UINT8)(CfgRead (Cfg, RootBus, 0, 0, PCI_CACHELINE_SIZE_OFFSET) >> 16);
  if ((HeaderType & HEADER_LAYOUT_CODE) != HEADER_TYPE_PCI_TO_PCI_BRIDGE) {
    return 0;
  }

  State.Cfg        = Cfg;
  State.NextBus    = RootBus + 1;
  State.BusLimit   = BusLimit;
  State.OutOfBuses = FALSE;
  State.SavedCount = 0;

  Largest = ScanBridge (&State, RootBus, 0, 0, 0);

  while (State.SavedCount > 0) {
    State.SavedCount--;
    Cfg->Write32 (
           Cfg->Context,
           State.Saved[State.SavedCount].Bus,
           State.Saved[State.SavedCount].Device,
           State.Saved[State.SavedCount].Function,
           PCI_BRIDGE_PRIMARY_BUS_REGISTER_OFFSET,
           State.Saved[State.SavedCount].BusNumbers
           );
  }

  return Largest;
}

BOOLEAN
PcieWindowFits (
  IN UINT64  Base,
  IN UINT64  Size,
  IN UINT64  BarSize
  )
{
  UINT64  Start;

  if (BarSize == 0) {
    return (BOOLEAN)(Size >= PCIE_WINDOW_HEADROOM);
  }

  if (((BarSize & (BarSize - 1)) != 0) || (BarSize > Size)) {
    return FALSE;
  }

  Start = (Base + BarSize - 1) & ~(BarSize - 1);
  if ((Start < Base) || (Start - Base > Size - BarSize)) {
    return FALSE;
  }

  return (BOOLEAN)(Size - BarSize >= PCIE_WINDOW_HEADROOM);
}

PCIE_WINDOW_LAYOUT
PcieWindowSelect (
  IN PCIE_WINDOW_LAYOUT  Forced,
  IN UINT64              MaxRebar
  )
{
  UINTN  Layout;

  if ((Forced != PcieWindowLayoutAuto) && ((UINTN)Forced < PcieWindowLayoutCount)) {
    return Forced;
  }

  for (Layout = PcieWindowLayoutStock16G; Layout < PcieWindowLayoutCount; Layout++) {
    if (PcieWindowFits (PCIE_WINDOW_RB0_MEM64_BASE, mLayoutSize[Layout], MaxRebar)) {
      return (PCIE_WINDOW_LAYOUT)Layout;
    }
  }

  //
  // Nothing holds the BAR at full size; the largest window still lets the
  // OS resize it to the biggest size that fits.
  //
  return (PCIE_WINDOW_LAYOUT)(PcieWindowLayoutCount - 1);
}

/**
  Bits of the largest naturally aligned block that starts at Base and does
  not pass Base + Size.
**/
STATIC
UINTN
AlignedBlockBits (
  IN UINT64  Base,
  IN UINT64  Size
  )
{
  UINTN  Bits;

  ASSERT (Size != 0);

  Bits = 63;
  while ((Bits > 0) && (((Size >> Bits) & 1) == 0)) {
    Bits--;
  }

  while ((Bits > 0) && (((Base >> Bits) << Bits) != Base)) {
    Bits--;
  }

  return Bits;
}

BOOLEAN
PcieWindowPlanAtu (
  IN  CONST PCIE_ATU_OB_REGION  Regions[PCIE_ATU_OB_REGION_COUNT],
  IN  UINT64                    Base,
  IN  UINT64                    StockSize,
  IN  UINT64                    Size,
  OUT PCIE_ATU_PLAN             *Plan
  )
{
  UINTN   Index;
  UINTN   Stock;
  UINTN   Free;
  UINTN   FirstBits;
  UINTN   SecondBits;
  UINT64  SecondBase;
  UINT64  RegionBase;
  UINT64  RegionEnd;

  Plan->WidenIndex = PCIE_ATU_NO_REGION;
  Plan->AddIndex   = PCIE_ATU_NO_REGION;

  if ((Size < StockSize) || (StockSize == 0) || ((StockSize & (StockSize - 1)) != 0) ||
      (Base + Size < Base))
  {
    return FALSE;
  }

  if (Size == StockSize) {
    return TRUE;
  }

  //
  // The stock window must be translated by exactly one region: base and size
  // as described, memory TLPs, identity (64 passed-through bits) PCI address.
  //
  Stock = PCIE_ATU_OB_REGION_COUNT;
  Free  = PCIE_ATU_OB_REGION_COUNT;
  for (Index = 0; Index < PCIE_ATU_OB_REGION_COUNT; Index++) {
    if ((Regions[Index].CpuAddr0 == 0) && (Regions[Index].CpuAddr1 == 0) &&
        (Regions[Index].Desc0 == 0) && (Regions[Index].Desc1 == 0))
    {
      if (Free == PCIE_ATU_OB_REGION_COUNT) {
        Free = Index;
      }

      continue;
    }

    RegionBase = ATU_BASE (Regions[Index].CpuAddr0, Regions[Index].CpuAddr1);
    if ((RegionBase == Base) &&
        (LShiftU64 (1, ATU_NBITS (Regions[Index].CpuAddr0)) == StockSize) &&
        (ATU_DESC0_TYPE (Regions[Index].Desc0) == ATU_TYPE_MEM) &&
        (Regions[Index].PciAddr0 == ATU_PCI_IDENTITY) && (Regions[Index].PciAddr1 == 0) &&
        (Stock == PCIE_ATU_OB_REGION_COUNT))
    {
      Stock = Index;
      continue;
    }

    //
    // Any other region reaching into the grown window would compete with it.
    //
    RegionEnd = RegionBase + LShiftU64 (1, ATU_NBITS (Regions[Index].CpuAddr0));
    if ((RegionBase < Base + Size) && (RegionEnd > Base)) {
      return FALSE;
    }
  }

  if (Stock == PCIE_ATU_OB_REGION_COUNT) {
    return FALSE;
  }

  FirstBits = AlignedBlockBits (Base, Size);
  if (LShiftU64 (1, FirstBits) < StockSize) {
    return FALSE;
  }

  Plan->WidenIndex    = (UINT8)Stock;
  Plan->WidenCpuAddr0 = (Regions[Stock].CpuAddr0 & ~0x3FU) | (UINT32)(FirstBits - 1);

  SecondBase = Base + LShiftU64 (1, FirstBits);
  if (SecondBase == Base + Size) {
    return TRUE;
  }

  SecondBits = AlignedBlockBits (SecondBase, Base + Size - SecondBase);
  if ((SecondBase + LShiftU64 (1, SecondBits) != Base + Size) || (Free == PCIE_ATU_OB_REGION_COUNT)) {
    Plan->WidenIndex = PCIE_ATU_NO_REGION;
    return FALSE;
  }

  Plan->AddIndex       = (UINT8)Free;
  Plan->Add            = Regions[Stock];
  Plan->Add.CpuAddr0   = ((UINT32)SecondBase & 0xFFFFFF00) | (UINT32)(SecondBits - 1);
  Plan->Add.CpuAddr1   = (UINT32)(SecondBase >> 32);
  return TRUE;
}
