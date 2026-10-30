set(RNG_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../RngLib.c"
  "${CMAKE_CURRENT_LIST_DIR}/RngMock.c")
sky1_host_test(rng_unit SOURCES RngTest.c ${RNG_SOURCES})
sky1_host_test(rng_unit_release SOURCES RngTest.c ${RNG_SOURCES} DEFINES MDEPKG_NDEBUG)
sky1_host_fuzz(trng_response_fuzz SOURCES TrngResponseFuzz.c ${RNG_SOURCES})
