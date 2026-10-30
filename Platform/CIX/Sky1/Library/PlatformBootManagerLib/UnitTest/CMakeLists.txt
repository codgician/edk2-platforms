sky1_host_test(bootgate_test SOURCES BootGateTest.c
  "${EDK_ROOT}/MdePkg/Library/BasePrintLib/PrintLib.c"
  "${EDK_ROOT}/MdePkg/Library/BasePrintLib/PrintLibInternal.c"
  INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/Include" "${EDK_ROOT}/EmbeddedPkg/Include")
target_compile_options(bootgate_test PRIVATE "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/BootGatePcd.h" -ffunction-sections -fdata-sections)
target_link_options(bootgate_test PRIVATE -Wl,--gc-sections)
sky1_host_fuzz(bootcap_fuzz SOURCES BootCapFuzz.c)
