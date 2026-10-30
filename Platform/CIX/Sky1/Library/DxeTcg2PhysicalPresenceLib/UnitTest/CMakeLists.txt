set(PP_SOURCES ../DxeTcg2PhysicalPresenceLib.c ../../../Drivers/TPMVariableDxe/TPMVariableDxe.c PpMocks.c
  "${EDK_ROOT}/MdePkg/Library/BasePrintLib/PrintLib.c"
  "${EDK_ROOT}/MdePkg/Library/BasePrintLib/PrintLibInternal.c")
sky1_host_test(pp_test SOURCES PpTest.c ${PP_SOURCES})
target_compile_options(pp_test PRIVATE "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/PpAutoGen.h" -ffunction-sections -fdata-sections)
target_link_options(pp_test PRIVATE -Wl,--gc-sections)
sky1_host_fuzz(pp_fuzz SOURCES PpFuzz.c ${PP_SOURCES})
target_compile_options(pp_fuzz PRIVATE "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/PpAutoGen.h" -ffunction-sections -fdata-sections)
target_link_options(pp_fuzz PRIVATE -Wl,--gc-sections)
target_link_options(pp_test PRIVATE -Wl,--wrap=AllocatePool)
target_link_options(pp_fuzz PRIVATE -Wl,--wrap=AllocatePool)
sky1_host_test(pp_test_release SOURCES PpTest.c ${PP_SOURCES} DEFINES MDEPKG_NDEBUG)
target_compile_options(pp_test_release PRIVATE "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/PpAutoGen.h" -ffunction-sections -fdata-sections)
target_link_options(pp_test_release PRIVATE -Wl,--gc-sections -Wl,--wrap=AllocatePool)
