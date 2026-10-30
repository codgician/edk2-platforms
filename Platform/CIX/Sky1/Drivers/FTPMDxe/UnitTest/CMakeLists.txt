# SPDX-License-Identifier: BSD-2-Clause-Patent
# The cap path must use the resident transport even when no protocol exists.
set(FTPM_DRIVER_SOURCES
  DriverHarness.c
  ../ResidentTpm2DeviceLib/ResidentTpm2DeviceLib.c
  "${EDK_ROOT}/SecurityPkg/Library/Tpm2CommandLib/Tpm2Capability.c"
  "${EDK_ROOT}/MdePkg/Library/BasePrintLib/PrintLib.c"
  "${EDK_ROOT}/MdePkg/Library/BasePrintLib/PrintLibInternal.c")
sky1_host_test(ftpmdxe_test
  SOURCES DriverTest.c ${FTPM_DRIVER_SOURCES}
  INCLUDES "${CMAKE_CURRENT_LIST_DIR}/Include")
sky1_host_fuzz(response_fuzz
  SOURCES ResponseFuzz.c ${FTPM_DRIVER_SOURCES}
  INCLUDES "${CMAKE_CURRENT_LIST_DIR}/Include")
foreach(TARGET ftpmdxe_test response_fuzz)
  target_compile_options(${TARGET} PRIVATE
    "SHELL:-include ${CMAKE_CURRENT_LIST_DIR}/PrintPcd.h"
    -ffunction-sections -fdata-sections)
  target_link_options(${TARGET} PRIVATE -Wl,--gc-sections)
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/Setup.cmake")

sky1_host_fuzz(setup_fuzz
  SOURCES SetupFuzz.c SetupSource.c
  INCLUDES "${SKY1_ROOT}/Include/Library")
