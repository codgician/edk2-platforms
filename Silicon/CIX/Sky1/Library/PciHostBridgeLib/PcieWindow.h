/** @file
  Sizing of the 64-bit MMIO window of the Sky1 x8 root bridge (RB0).

  RB0 decodes from a fixed base. A larger window keeps that base and every
  other bridge untouched and only grows RB0 upward, into address space that
  nothing else on the SoC uses (DRAM high starts at 0x80_00000000).

  Everything here is free of UEFI services: configuration space and the
  outbound address translation registers are passed in, so the policy can be
  exercised on the build host.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef SKY1_PCIE_WINDOW_H_
#define SKY1_PCIE_WINDOW_H_

#include <Uefi.h>

#define PCIE_WINDOW_RB0_MEM64_BASE  0x1800000000ULL
#define PCIE_WINDOW_RB0_STOCK_SIZE  SIZE_16GB

//
// Space a layout must leave beside the largest resizable BAR for the other
// 64-bit BARs and bridge windows below RB0.
//
#define PCIE_WINDOW_HEADROOM  SIZE_512MB

//
// Values of PcdPcieRb0Mem64Layout. Auto picks the smallest layout that holds
// the largest resizable BAR found below RB0; the others force a layout.
//
typedef enum {
  PcieWindowLayoutAuto = 0,
  PcieWindowLayoutStock16G,     // ends 0x1B_FFFFFFFF
  PcieWindowLayout32G,          // ends 0x1F_FFFFFFFF
  PcieWindowLayout64G,          // ends 0x27_FFFFFFFF
  PcieWindowLayoutCount
} PCIE_WINDOW_LAYOUT;

typedef
UINT32
(*PCIE_WINDOW_CFG_READ32)(
  IN VOID    *Context,
  IN UINT8   Bus,
  IN UINT8   Device,
  IN UINT8   Function,
  IN UINT16  Offset
  );

typedef
VOID
(*PCIE_WINDOW_CFG_WRITE32)(
  IN VOID    *Context,
  IN UINT8   Bus,
  IN UINT8   Device,
  IN UINT8   Function,
  IN UINT16  Offset,
  IN UINT32  Value
  );

typedef struct {
  PCIE_WINDOW_CFG_READ32     Read32;
  PCIE_WINDOW_CFG_WRITE32    Write32;
  VOID                       *Context;
} PCIE_WINDOW_CFG;

//
// One outbound region of the Cadence HPA address translation unit, in
// register order (CPU address, descriptors, PCI address, control).
//
typedef struct {
  UINT32    CpuAddr0;
  UINT32    CpuAddr1;
  UINT32    Desc0;
  UINT32    Desc1;
  UINT32    PciAddr0;
  UINT32    PciAddr1;
  UINT32    Ctrl0;
} PCIE_ATU_OB_REGION;

#define PCIE_ATU_OB_REGION_COUNT  8
#define PCIE_ATU_NO_REGION        0xFF

//
// Register writes that make the outbound translation cover a grown window:
// the region that already translates the stock window is widened in place,
// and at most one free region is added for the part that does not fit a
// single naturally aligned block.
//
typedef struct {
  UINT8                 WidenIndex;
  UINT32                WidenCpuAddr0;
  UINT8                 AddIndex;
  PCIE_ATU_OB_REGION    Add;
} PCIE_ATU_PLAN;

/**
  Size of RB0's 64-bit window in a layout, or 0 for Auto and invalid values.
**/
UINT64
PcieWindowLayoutSize (
  IN PCIE_WINDOW_LAYOUT  Layout
  );

/**
  Offset of an extended capability, or 0 when absent. The walk stays inside
  extended configuration space and is bounded, so a looping or corrupt list
  ends the search instead of hanging it.
**/
UINT16
PcieWindowFindExtCapability (
  IN CONST PCIE_WINDOW_CFG  *Cfg,
  IN UINT8                  Bus,
  IN UINT8                  Device,
  IN UINT8                  Function,
  IN UINT16                 CapabilityId
  );

/**
  Largest size any BAR of one function can be resized to, or 0 when the
  function has no usable Resizable BAR capability.
**/
UINT64
PcieWindowFunctionMaxRebar (
  IN CONST PCIE_WINDOW_CFG  *Cfg,
  IN UINT8                  Bus,
  IN UINT8                  Device,
  IN UINT8                  Function
  );

/**
  Largest resizable BAR of every function below the root port at
  RootBus:00.0, following switches. Bridges get temporary bus numbers in
  (RootBus, BusLimit] for the walk; every bus number register written is
  restored before returning.
**/
UINT64
PcieWindowHierarchyMaxRebar (
  IN CONST PCIE_WINDOW_CFG  *Cfg,
  IN UINT8                  RootBus,
  IN UINT8                  BusLimit
  );

/**
  TRUE when a naturally aligned BarSize block fits in [Base, Base + Size)
  and leaves at least PCIE_WINDOW_HEADROOM of the window unused.
**/
BOOLEAN
PcieWindowFits (
  IN UINT64  Base,
  IN UINT64  Size,
  IN UINT64  BarSize
  );

/**
  The layout to use: Forced unless it is Auto, else the smallest layout that
  fits MaxRebar, else the largest layout.
**/
PCIE_WINDOW_LAYOUT
PcieWindowSelect (
  IN PCIE_WINDOW_LAYOUT  Forced,
  IN UINT64              MaxRebar
  );

/**
  Plan the outbound translation for [Base, Base + Size) from a snapshot of
  the regions programmed for the stock window [Base, Base + StockSize).

  @retval TRUE   Plan holds the writes (indices PCIE_ATU_NO_REGION when none).
  @retval FALSE  The programming is not the expected identity translation of
                 the stock window, or no free region is available.
**/
BOOLEAN
PcieWindowPlanAtu (
  IN  CONST PCIE_ATU_OB_REGION  Regions[PCIE_ATU_OB_REGION_COUNT],
  IN  UINT64                    Base,
  IN  UINT64                    StockSize,
  IN  UINT64                    Size,
  OUT PCIE_ATU_PLAN             *Plan
  );

#endif
