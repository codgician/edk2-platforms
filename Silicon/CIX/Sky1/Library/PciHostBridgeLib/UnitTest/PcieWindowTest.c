/** @file
  Host tests for the RB0 64-bit window policy: layout choice, BAR fit,
  the bounded capability walks, the hierarchy scan and the outbound
  translation plan.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>
#include <IndustryStandard/Pci.h>
#include "PcieWindow.h"

#define ROOT_BUS   0xC0
#define BUS_LIMIT  0xFF
#define BASE       PCIE_WINDOW_RB0_MEM64_BASE

//
// A tiny PCI topology: a node is one function; bridge nodes route type-1
// configuration cycles by the bus numbers software wrote into them.
//
typedef struct NODE NODE;
struct NODE {
  UINT8     Device;
  UINT8     Function;
  UINT32    Cfg[1024];
  NODE      *Children[8];
  UINTN     ChildCount;
};

typedef struct {
  NODE     *Root;
  UINTN    Reads;
} TOPOLOGY;

STATIC NODE      mNodes[16];
STATIC UINTN     mNodeCount;
STATIC TOPOLOGY  mTopology;

STATIC NODE *
NewNode (
  UINT8   Device,
  UINT8   Function,
  UINT16  VendorId,
  UINT8   HeaderType
  )
{
  NODE  *Node;

  assert_true (mNodeCount < ARRAY_SIZE (mNodes));
  Node = &mNodes[mNodeCount++];
  memset (Node, 0, sizeof (*Node));
  Node->Device   = Device;
  Node->Function = Function;
  Node->Cfg[0]   = VendorId | (0x1234U << 16);
  Node->Cfg[3]   = (UINT32)HeaderType << 16;
  return Node;
}

STATIC VOID
AddChild (
  NODE  *Bridge,
  NODE  *Child
  )
{
  assert_true (Bridge->ChildCount < ARRAY_SIZE (Bridge->Children));
  Bridge->Children[Bridge->ChildCount++] = Child;
}

//
// PCI Express capability with the given port type at 0x40.
//
STATIC VOID
SetPortType (
  NODE   *Node,
  UINT8  PortType
  )
{
  Node->Cfg[1]   |= (UINT32)EFI_PCI_STATUS_CAPABILITY << 16;
  Node->Cfg[0x34 / 4] = 0x40;
  Node->Cfg[0x40 / 4] = EFI_PCI_CAPABILITY_ID_PCIEXP | ((UINT32)PortType << 20);
}

//
// Resizable BAR capability at Offset advertising Sizes (bit n = 2^(n+16)
// bytes in the capability register) for one BAR.
//
STATIC VOID
SetRebar (
  NODE    *Node,
  UINT16  Offset,
  UINT32  CapabilityBits,
  UINT32  ControlHighBits
  )
{
  Node->Cfg[Offset / 4]       = 0x0015 | (1U << 16);
  Node->Cfg[Offset / 4 + 1]   = CapabilityBits;
  Node->Cfg[Offset / 4 + 2]   = (1U << 5) | ControlHighBits;
}

STATIC NODE *
Resolve (
  NODE   *Bridge,
  UINT8  Bus,
  UINT8  Device,
  UINT8  Function
  )
{
  UINT8  Secondary;
  UINT8  Subordinate;
  UINTN  Index;
  NODE   *Found;

  Secondary   = (UINT8)(Bridge->Cfg[0x18 / 4] >> 8);
  Subordinate = (UINT8)(Bridge->Cfg[0x18 / 4] >> 16);
  if (Secondary == 0) {
    return NULL;
  }

  for (Index = 0; Index < Bridge->ChildCount; Index++) {
    if (Bus == Secondary) {
      if ((Bridge->Children[Index]->Device == Device) && (Bridge->Children[Index]->Function == Function)) {
        return Bridge->Children[Index];
      }
    } else if ((Bus > Secondary) && (Bus <= Subordinate)) {
      Found = Resolve (Bridge->Children[Index], Bus, Device, Function);
      if (Found != NULL) {
        return Found;
      }
    }
  }

  return NULL;
}

STATIC NODE *
Lookup (
  UINT8  Bus,
  UINT8  Device,
  UINT8  Function
  )
{
  if (Bus == ROOT_BUS) {
    return ((Device == 0) && (Function == 0)) ? mTopology.Root : NULL;
  }

  return Resolve (mTopology.Root, Bus, Device, Function);
}

STATIC UINT32
MockRead32 (
  VOID    *Context,
  UINT8   Bus,
  UINT8   Device,
  UINT8   Function,
  UINT16  Offset
  )
{
  NODE  *Node;

  (VOID)Context;
  assert_int_equal (Offset & 3, 0);
  assert_true (Offset < 0x1000);
  mTopology.Reads++;
  Node = Lookup (Bus, Device, Function);
  return (Node == NULL) ? MAX_UINT32 : Node->Cfg[Offset / 4];
}

STATIC VOID
MockWrite32 (
  VOID    *Context,
  UINT8   Bus,
  UINT8   Device,
  UINT8   Function,
  UINT16  Offset,
  UINT32  Value
  )
{
  NODE  *Node;

  (VOID)Context;
  Node = Lookup (Bus, Device, Function);
  assert_non_null (Node);
  Node->Cfg[Offset / 4] = Value;
}

STATIC CONST PCIE_WINDOW_CFG  mCfg = { MockRead32, MockWrite32, NULL };

STATIC NODE *
NewRootPort (
  VOID
  )
{
  NODE  *Root;

  mNodeCount       = 0;
  mTopology.Reads  = 0;
  Root             = NewNode (0, 0, 0x17CD, HEADER_TYPE_PCI_TO_PCI_BRIDGE);
  SetPortType (Root, 0x4);
  mTopology.Root   = Root;
  return Root;
}

//
// ---- Window fit and layout selection ------------------------------------
//

STATIC VOID
EmptySlotKeepsStock (
  VOID  **State
  )
{
  (VOID)State;
  assert_int_equal (PcieWindowSelect (PcieWindowLayoutAuto, 0), PcieWindowLayoutStock16G);
  assert_int_equal (PcieWindowLayoutSize (PcieWindowLayoutStock16G), SIZE_16GB);
  assert_int_equal (PcieWindowLayoutSize (PcieWindowLayoutAuto), 0);
  assert_int_equal (PcieWindowLayoutSize (PcieWindowLayoutCount), 0);
}

STATIC VOID
AutoPicksSmallestLayoutWithHeadroom (
  VOID  **State
  )
{
  (VOID)State;
  assert_int_equal (PcieWindowSelect (PcieWindowLayoutAuto, SIZE_256MB), PcieWindowLayoutStock16G);
  assert_int_equal (PcieWindowSelect (PcieWindowLayoutAuto, SIZE_8GB), PcieWindowLayoutStock16G);
  // A 16 GiB BAR fills the stock window and leaves no headroom.
  assert_int_equal (PcieWindowSelect (PcieWindowLayoutAuto, SIZE_16GB), PcieWindowLayout32G);
  assert_int_equal (PcieWindowSelect (PcieWindowLayoutAuto, SIZE_32GB), PcieWindowLayout64G);
  // Nothing holds 64 GiB at its alignment: the largest layout, for resizing.
  assert_int_equal (PcieWindowSelect (PcieWindowLayoutAuto, SIZE_64GB), PcieWindowLayout64G);
  assert_int_equal (PcieWindowSelect (PcieWindowLayoutAuto, 1ULL << 63), PcieWindowLayout64G);
}

STATIC VOID
ForcedLayoutWins (
  VOID  **State
  )
{
  (VOID)State;
  assert_int_equal (PcieWindowSelect (PcieWindowLayout32G, 0), PcieWindowLayout32G);
  assert_int_equal (PcieWindowSelect (PcieWindowLayoutStock16G, SIZE_32GB), PcieWindowLayoutStock16G);
  assert_int_equal (PcieWindowSelect ((PCIE_WINDOW_LAYOUT)7, SIZE_16GB), PcieWindowLayout32G);
}

STATIC VOID
FitBoundaries (
  VOID  **State
  )
{
  (VOID)State;
  // Exactly the headroom left over fits, one byte less does not.
  assert_true (PcieWindowFits (BASE, SIZE_16GB, SIZE_8GB));
  assert_true (PcieWindowFits (0, SIZE_1GB + SIZE_512MB, SIZE_1GB));
  assert_false (PcieWindowFits (0, SIZE_1GB + SIZE_512MB - 1, SIZE_1GB));
  // Alignment loss: an 8 GiB BAR in a window starting 4 GiB in.
  assert_false (PcieWindowFits (SIZE_4GB, SIZE_8GB + SIZE_2GB, SIZE_8GB));
  assert_true (PcieWindowFits (SIZE_4GB, SIZE_16GB, SIZE_8GB));
  assert_false (PcieWindowFits (BASE, SIZE_16GB, SIZE_8GB + 1));
  assert_false (PcieWindowFits (BASE, SIZE_16GB, SIZE_32GB));
  assert_false (PcieWindowFits (MAX_UINT64 - SIZE_1GB, SIZE_1GB, SIZE_2GB));
}

//
// ---- Capability walks ----------------------------------------------------
//

STATIC VOID
ExtendedWalkFindsChainedCapability (
  VOID  **State
  )
{
  NODE  *Root;
  NODE  *Gpu;

  (VOID)State;
  Root = NewRootPort ();
  Root->Cfg[0x18 / 4] = (BUS_LIMIT << 16) | ((ROOT_BUS + 1) << 8) | ROOT_BUS;
  Gpu = NewNode (0, 0, 0x1002, 0);
  AddChild (Root, Gpu);
  // AER at 0x100 -> (reserved low bits set) -> ReBAR at 0x200.
  Gpu->Cfg[0x100 / 4] = 0x0001 | (0x203U << 20);
  SetRebar (Gpu, 0x200, 1U << 18, 0);
  assert_int_equal (PcieWindowFindExtCapability (&mCfg, ROOT_BUS + 1, 0, 0, 0x15), 0x200);
  assert_int_equal (PcieWindowFindExtCapability (&mCfg, ROOT_BUS + 1, 0, 0, 0x0B), 0);
  assert_int_equal (PcieWindowFunctionMaxRebar (&mCfg, ROOT_BUS + 1, 0, 0), SIZE_16GB);
}

STATIC VOID
ExtendedWalkIsBounded (
  VOID  **State
  )
{
  NODE  *Root;
  NODE  *Dev;

  (VOID)State;
  Root = NewRootPort ();
  Root->Cfg[0x18 / 4] = (BUS_LIMIT << 16) | ((ROOT_BUS + 1) << 8) | ROOT_BUS;
  Dev = NewNode (0, 0, 0x1002, 0);
  AddChild (Root, Dev);

  // 0x100 -> 0x200 -> 0x100 ...: terminates within the hop bound.
  Dev->Cfg[0x100 / 4] = 0x0001 | (0x200U << 20);
  Dev->Cfg[0x200 / 4] = 0x0002 | (0x100U << 20);
  mTopology.Reads     = 0;
  assert_int_equal (PcieWindowFindExtCapability (&mCfg, ROOT_BUS + 1, 0, 0, 0x15), 0);
  assert_true (mTopology.Reads <= (0x1000 - 0x100) / 4);

  // A self loop.
  Dev->Cfg[0x100 / 4] = 0x0001 | (0x100U << 20);
  assert_int_equal (PcieWindowFindExtCapability (&mCfg, ROOT_BUS + 1, 0, 0, 0x15), 0);

  // A pointer back into standard configuration space ends the walk.
  Dev->Cfg[0x100 / 4] = 0x0001 | (0x040U << 20);
  Dev->Cfg[0x40 / 4]  = 0x0015;
  assert_int_equal (PcieWindowFindExtCapability (&mCfg, ROOT_BUS + 1, 0, 0, 0x15), 0);

  // All-ones (no device) and zero headers.
  assert_int_equal (PcieWindowFindExtCapability (&mCfg, ROOT_BUS + 2, 0, 0, 0x15), 0);
  Dev->Cfg[0x100 / 4] = 0;
  assert_int_equal (PcieWindowFindExtCapability (&mCfg, ROOT_BUS + 1, 0, 0, 0x15), 0);
}

STATIC VOID
RebarSizeDecoding (
  VOID  **State
  )
{
  NODE  *Root;
  NODE  *Dev;

  (VOID)State;
  Root = NewRootPort ();
  Root->Cfg[0x18 / 4] = (BUS_LIMIT << 16) | ((ROOT_BUS + 1) << 8) | ROOT_BUS;
  Dev = NewNode (0, 0, 0x10DE, 0);
  AddChild (Root, Dev);

  // 1 MiB .. 256 MiB, the largest wins.
  SetRebar (Dev, 0x100, 0x1FF0, 0);
  assert_int_equal (PcieWindowFunctionMaxRebar (&mCfg, ROOT_BUS + 1, 0, 0), SIZE_256MB);

  // Control register bits 16.. continue the series at 256 TiB.
  SetRebar (Dev, 0x100, 0x10, 1U << 16);
  assert_int_equal (PcieWindowFunctionMaxRebar (&mCfg, ROOT_BUS + 1, 0, 0), 1ULL << 48);

  // Two BARs: the second one is larger.
  SetRebar (Dev, 0x100, 1U << 12, 0);
  Dev->Cfg[0x108 / 4] = 2U << 5;
  Dev->Cfg[0x10C / 4] = 1U << 20;
  assert_int_equal (PcieWindowFunctionMaxRebar (&mCfg, ROOT_BUS + 1, 0, 0), SIZE_64GB);

  // Invalid BAR counts are ignored.
  Dev->Cfg[0x108 / 4] = 0;
  assert_int_equal (PcieWindowFunctionMaxRebar (&mCfg, ROOT_BUS + 1, 0, 0), 0);
  Dev->Cfg[0x108 / 4] = 7U << 5;
  assert_int_equal (PcieWindowFunctionMaxRebar (&mCfg, ROOT_BUS + 1, 0, 0), 0);

  // A capability whose BAR entries would run past configuration space.
  memset (&Dev->Cfg[0x100 / 4], 0, 16);
  Dev->Cfg[0x100 / 4] = 0x0001 | (0xFF0U << 20);
  Dev->Cfg[0xFF0 / 4] = 0x0015;
  Dev->Cfg[0xFF8 / 4] = 6U << 5;
  Dev->Cfg[0xFF4 / 4] = 1U << 18;
  assert_int_equal (PcieWindowFunctionMaxRebar (&mCfg, ROOT_BUS + 1, 0, 0), 0);
}

//
// ---- Hierarchy scan ------------------------------------------------------
//

STATIC VOID
ScanFindsRebarOnLaterFunctionAndRestores (
  VOID  **State
  )
{
  NODE    *Root;
  NODE    *Fn0;
  NODE    *Fn1;
  UINT32  Original;

  (VOID)State;
  Root     = NewRootPort ();
  Original = 0x00000000;
  Root->Cfg[0x18 / 4] = Original;
  Fn0 = NewNode (0, 0, 0x10DE, HEADER_TYPE_MULTI_FUNCTION);
  Fn1 = NewNode (0, 1, 0x10DE, 0);
  AddChild (Root, Fn0);
  AddChild (Root, Fn1);
  SetRebar (Fn1, 0x100, 1U << 18, 0);

  assert_int_equal (PcieWindowHierarchyMaxRebar (&mCfg, ROOT_BUS, BUS_LIMIT), SIZE_16GB);
  assert_int_equal (Root->Cfg[0x18 / 4], Original);
}

STATIC VOID
ScanIgnoresFunctionsOfSingleFunctionDevice (
  VOID  **State
  )
{
  NODE  *Root;
  NODE  *Fn1;

  (VOID)State;
  Root = NewRootPort ();
  AddChild (Root, NewNode (0, 0, 0x10DE, 0));
  Fn1 = NewNode (0, 1, 0x10DE, 0);
  AddChild (Root, Fn1);
  SetRebar (Fn1, 0x100, 1U << 18, 0);

  assert_int_equal (PcieWindowHierarchyMaxRebar (&mCfg, ROOT_BUS, BUS_LIMIT), 0);
}

STATIC VOID
ScanFollowsSwitchAndRestores (
  VOID  **State
  )
{
  NODE    *Root;
  NODE    *Upstream;
  NODE    *Down0;
  NODE    *Down1;
  NODE    *Gpu;
  NODE    *Audio;
  NODE    *Other;
  UINT32  RootBuses;

  (VOID)State;
  // Root -> switch upstream port -> two downstream ports; the GPU (with a
  // 32 GiB resizable BAR) sits behind the second one, as on Radeon cards.
  Root      = NewRootPort ();
  RootBuses = (0xC5U << 16) | (0xC1U << 8) | ROOT_BUS;
  Root->Cfg[0x18 / 4] = RootBuses;
  Upstream = NewNode (0, 0, 0x1002, HEADER_TYPE_PCI_TO_PCI_BRIDGE);
  SetPortType (Upstream, 0x5);
  Upstream->Cfg[0x18 / 4] = 0x00ABCDEF;
  Down0 = NewNode (0, 0, 0x1002, HEADER_TYPE_PCI_TO_PCI_BRIDGE);
  Down1 = NewNode (1, 0, 0x1002, HEADER_TYPE_PCI_TO_PCI_BRIDGE);
  SetPortType (Down0, 0x6);
  SetPortType (Down1, 0x6);
  Gpu   = NewNode (0, 0, 0x1002, HEADER_TYPE_MULTI_FUNCTION);
  Audio = NewNode (0, 1, 0x1002, 0);
  Gpu->Cfg[0x100 / 4] = 0x000B | (0x200U << 20);
  SetRebar (Gpu, 0x200, 1U << 19, 0);
  Other = NewNode (0, 0, 0x8086, 0);
  SetRebar (Other, 0x100, 1U << 10, 0);
  AddChild (Root, Upstream);
  AddChild (Upstream, Down0);
  AddChild (Upstream, Down1);
  AddChild (Down0, Other);
  AddChild (Down1, Gpu);
  AddChild (Down1, Audio);

  assert_int_equal (PcieWindowHierarchyMaxRebar (&mCfg, ROOT_BUS, BUS_LIMIT), SIZE_32GB);
  assert_int_equal (Root->Cfg[0x18 / 4], RootBuses);
  assert_int_equal (Upstream->Cfg[0x18 / 4], 0x00ABCDEF);
  assert_int_equal (Down0->Cfg[0x18 / 4], 0);
  assert_int_equal (Down1->Cfg[0x18 / 4], 0);
  assert_int_equal (PcieWindowSelect (PcieWindowLayoutAuto, SIZE_32GB), PcieWindowLayout64G);
}

STATIC VOID
ScanStopsWhenBusesRunOut (
  VOID  **State
  )
{
  NODE   *Root;
  NODE   *Parent;
  NODE   *Bridge;
  UINTN  Index;

  (VOID)State;
  // A chain of bridges deeper than the bus range; the endpoint at the end
  // is out of reach and the walk must still end and restore everything.
  Root   = NewRootPort ();
  Parent = Root;
  for (Index = 0; Index < 4; Index++) {
    Bridge = NewNode (0, 0, 0x1B21, HEADER_TYPE_PCI_TO_PCI_BRIDGE);
    SetPortType (Bridge, 0x6);
    AddChild (Parent, Bridge);
    Parent = Bridge;
  }

  Bridge = NewNode (0, 0, 0x1002, 0);
  SetRebar (Bridge, 0x100, 1U << 18, 0);
  AddChild (Parent, Bridge);

  assert_int_equal (PcieWindowHierarchyMaxRebar (&mCfg, ROOT_BUS, ROOT_BUS + 3), 0);
  for (Index = 0; Index < mNodeCount; Index++) {
    assert_int_equal (mNodes[Index].Cfg[0x18 / 4], 0);
  }

  // With enough buses the same chain is found.
  assert_int_equal (PcieWindowHierarchyMaxRebar (&mCfg, ROOT_BUS, BUS_LIMIT), SIZE_16GB);
}

STATIC VOID
ScanWithoutRootPort (
  VOID  **State
  )
{
  NODE  *Root;

  (VOID)State;
  Root = NewRootPort ();
  Root->Cfg[3] = 0;   // not a bridge
  assert_int_equal (PcieWindowHierarchyMaxRebar (&mCfg, ROOT_BUS, BUS_LIMIT), 0);
  Root->Cfg[0] = MAX_UINT32;
  assert_int_equal (PcieWindowHierarchyMaxRebar (&mCfg, ROOT_BUS, BUS_LIMIT), 0);
  assert_int_equal (PcieWindowHierarchyMaxRebar (&mCfg, ROOT_BUS, ROOT_BUS), 0);
}

//
// ---- Outbound translation plan --------------------------------------------
//

//
// The programming read back from the live x4 controller, moved to RB0: ECAM
// regions, the 32-bit window and the 16 GiB 64-bit window in region 3.
//
STATIC VOID
StockRegions (
  PCIE_ATU_OB_REGION  Regions[PCIE_ATU_OB_REGION_COUNT]
  )
{
  UINTN  Index;

  memset (Regions, 0, sizeof (PCIE_ATU_OB_REGION) * PCIE_ATU_OB_REGION_COUNT);
  Regions[0] = (PCIE_ATU_OB_REGION){ 0x2C000018, 0, 0x05010000, 0xC0000000, 0x0000001B, 0, 0x06000000 };
  Regions[1] = (PCIE_ATU_OB_REGION){ 0x2C000018, 0, 0x04010000, 0xC1000000, 0x0010001B, 0, 0x06000000 };
  Regions[2] = (PCIE_ATU_OB_REGION){ 0x6000001C, 0, 0x00000000, 0xC0000000, 0x0000001F, 0, 0x06000000 };
  Regions[3] = (PCIE_ATU_OB_REGION){ 0x00000021, 0x18, 0x00000000, 0xC0000000, 0x0000003F, 0, 0x06000000 };
  for (Index = 4; Index < PCIE_ATU_OB_REGION_COUNT; Index++) {
    Regions[Index].PciAddr0 = 0x7;
    Regions[Index].Ctrl0    = 0x06000000;
  }
}

STATIC VOID
AtuStockNeedsNoWrites (
  VOID  **State
  )
{
  PCIE_ATU_OB_REGION  Regions[PCIE_ATU_OB_REGION_COUNT];
  PCIE_ATU_PLAN       Plan;

  (VOID)State;
  StockRegions (Regions);
  assert_true (PcieWindowPlanAtu (Regions, BASE, SIZE_16GB, SIZE_16GB, &Plan));
  assert_int_equal (Plan.WidenIndex, PCIE_ATU_NO_REGION);
  assert_int_equal (Plan.AddIndex, PCIE_ATU_NO_REGION);
}

STATIC VOID
AtuWidensFor32G (
  VOID  **State
  )
{
  PCIE_ATU_OB_REGION  Regions[PCIE_ATU_OB_REGION_COUNT];
  PCIE_ATU_PLAN       Plan;

  (VOID)State;
  StockRegions (Regions);
  assert_true (PcieWindowPlanAtu (Regions, BASE, SIZE_16GB, SIZE_32GB, &Plan));
  assert_int_equal (Plan.WidenIndex, 3);
  assert_int_equal (Plan.WidenCpuAddr0, 0x22);
  assert_int_equal (Plan.AddIndex, PCIE_ATU_NO_REGION);
}

STATIC VOID
AtuSplits64GIntoAlignedRegions (
  VOID  **State
  )
{
  PCIE_ATU_OB_REGION  Regions[PCIE_ATU_OB_REGION_COUNT];
  PCIE_ATU_PLAN       Plan;

  (VOID)State;
  // 0x18_00000000 is only 32 GiB aligned: 0x18.. and 0x20.. take one each.
  StockRegions (Regions);
  assert_true (PcieWindowPlanAtu (Regions, BASE, SIZE_16GB, SIZE_64GB, &Plan));
  assert_int_equal (Plan.WidenIndex, 3);
  assert_int_equal (Plan.WidenCpuAddr0, 0x22);
  assert_int_equal (Plan.AddIndex, 4);
  assert_int_equal (Plan.Add.CpuAddr0, 0x22);
  assert_int_equal (Plan.Add.CpuAddr1, 0x20);
  assert_int_equal (Plan.Add.Desc0, Regions[3].Desc0);
  assert_int_equal (Plan.Add.Desc1, Regions[3].Desc1);
  assert_int_equal (Plan.Add.PciAddr0, 0x3F);
  assert_int_equal (Plan.Add.PciAddr1, 0);
  assert_int_equal (Plan.Add.Ctrl0, Regions[3].Ctrl0);
}

STATIC VOID
AtuRefusesUnexpectedProgramming (
  VOID  **State
  )
{
  PCIE_ATU_OB_REGION  Regions[PCIE_ATU_OB_REGION_COUNT];
  PCIE_ATU_PLAN       Plan;
  UINTN               Index;

  (VOID)State;
  // Translated (non-identity) PCI address.
  StockRegions (Regions);
  Regions[3].PciAddr1 = 1;
  assert_false (PcieWindowPlanAtu (Regions, BASE, SIZE_16GB, SIZE_32GB, &Plan));

  // Stock region of a different size.
  StockRegions (Regions);
  Regions[3].CpuAddr0 = 0x22;
  assert_false (PcieWindowPlanAtu (Regions, BASE, SIZE_16GB, SIZE_32GB, &Plan));

  // Not a memory region.
  StockRegions (Regions);
  Regions[3].Desc0 = 0x02000000;
  assert_false (PcieWindowPlanAtu (Regions, BASE, SIZE_16GB, SIZE_32GB, &Plan));

  // Another region already decodes inside the grown window.
  StockRegions (Regions);
  Regions[5].CpuAddr0 = 0x1F;
  Regions[5].CpuAddr1 = 0x1E;
  Regions[5].Desc1    = 0xC0000000;
  assert_false (PcieWindowPlanAtu (Regions, BASE, SIZE_16GB, SIZE_32GB, &Plan));

  // 64 GiB needs a free region.
  StockRegions (Regions);
  for (Index = 4; Index < PCIE_ATU_OB_REGION_COUNT; Index++) {
    Regions[Index].CpuAddr0 = 0x10 + (UINT32)Index * 0x100;
    Regions[Index].Desc1    = 0xC0000000;
  }

  assert_false (PcieWindowPlanAtu (Regions, BASE, SIZE_16GB, SIZE_64GB, &Plan));
  assert_int_equal (Plan.WidenIndex, PCIE_ATU_NO_REGION);
  assert_true (PcieWindowPlanAtu (Regions, BASE, SIZE_16GB, SIZE_32GB, &Plan));

  // Shrinking is never planned.
  StockRegions (Regions);
  assert_false (PcieWindowPlanAtu (Regions, BASE, SIZE_16GB, SIZE_8GB, &Plan));
}

int
main (
  void
  )
{
  const struct CMUnitTest  Tests[] = {
    cmocka_unit_test (EmptySlotKeepsStock),
    cmocka_unit_test (AutoPicksSmallestLayoutWithHeadroom),
    cmocka_unit_test (ForcedLayoutWins),
    cmocka_unit_test (FitBoundaries),
    cmocka_unit_test (ExtendedWalkFindsChainedCapability),
    cmocka_unit_test (ExtendedWalkIsBounded),
    cmocka_unit_test (RebarSizeDecoding),
    cmocka_unit_test (ScanFindsRebarOnLaterFunctionAndRestores),
    cmocka_unit_test (ScanIgnoresFunctionsOfSingleFunctionDevice),
    cmocka_unit_test (ScanFollowsSwitchAndRestores),
    cmocka_unit_test (ScanStopsWhenBusesRunOut),
    cmocka_unit_test (ScanWithoutRootPort),
    cmocka_unit_test (AtuStockNeedsNoWrites),
    cmocka_unit_test (AtuWidensFor32G),
    cmocka_unit_test (AtuSplits64GIntoAlignedRegions),
    cmocka_unit_test (AtuRefusesUnexpectedProgramming),
  };

  return cmocka_run_group_tests (Tests, NULL, NULL);
}
