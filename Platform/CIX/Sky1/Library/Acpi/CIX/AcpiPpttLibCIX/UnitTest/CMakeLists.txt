# SPDX-License-Identifier: BSD-2-Clause-Patent
# The PPTT layout is read back the way Linux drivers/acpi/pptt.c reads it.
sky1_host_test(pptt_topology_test
  SOURCES PpttTopologyTest.c "${CMAKE_CURRENT_LIST_DIR}/../PpttTopology.c"
  INCLUDES "${CMAKE_CURRENT_LIST_DIR}/.." "${PLAT_ROOT}/Silicon/CIX/Sky1")
