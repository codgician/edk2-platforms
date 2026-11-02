# SPDX-License-Identifier: BSD-2-Clause-Patent
sky1_host_test(pcie_window_unit
  SOURCES PcieWindowTest.c "${CMAKE_CURRENT_LIST_DIR}/../PcieWindow.c"
  INCLUDES "${CMAKE_CURRENT_LIST_DIR}/..")
