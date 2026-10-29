# SPDX-License-Identifier: BSD-2-Clause-Patent
sky1_host_test(ftpmdxe_test
  SOURCES DriverTest.c DriverHarness.c
    "${EDK_ROOT}/SecurityPkg/Library/Tpm2CommandLib/Tpm2Capability.c"
  INCLUDES "${CMAKE_CURRENT_LIST_DIR}/Include")
target_compile_options(ftpmdxe_test PRIVATE -ffunction-sections -fdata-sections)
target_link_options(ftpmdxe_test PRIVATE -Wl,--gc-sections)

sky1_host_fuzz(response_fuzz
  SOURCES ResponseFuzz.c DriverHarness.c
  INCLUDES "${CMAKE_CURRENT_LIST_DIR}/Include")

include("${CMAKE_CURRENT_LIST_DIR}/Setup.cmake")

sky1_host_fuzz(setup_fuzz
  SOURCES SetupFuzz.c SetupSource.c
  INCLUDES "${SKY1_ROOT}/Include/Library")
