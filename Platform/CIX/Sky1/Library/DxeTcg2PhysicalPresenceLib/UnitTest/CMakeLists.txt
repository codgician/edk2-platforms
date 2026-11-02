set(PP_SOURCES ../DxeTcg2PhysicalPresenceLib.c PpMocks.c)
sky1_host_test(pp_test PRELUDE PpAutoGen.h SOURCES PpTest.c ${PP_SOURCES})
sky1_host_fuzz(pp_fuzz PRELUDE PpAutoGen.h SOURCES PpFuzz.c ${PP_SOURCES})
