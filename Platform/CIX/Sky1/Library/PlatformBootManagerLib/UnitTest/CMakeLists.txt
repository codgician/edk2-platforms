sky1_host_test(bootgate_test PRELUDE BootGatePcd.h SOURCES BootGateTest.c
  INCLUDES "${EDK_ROOT}/EmbeddedPkg/Include")
sky1_host_fuzz(bootcap_fuzz SOURCES BootCapFuzz.c)
