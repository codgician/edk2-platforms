# Compile the production setup-routing library via the PCD-only adapter.
sky1_host_test(setup_test
  SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/SetupTest.c"
    "${CMAKE_CURRENT_LIST_DIR}/SetupSource.c"
  INCLUDES
    "${SKY1_ROOT}/Include/Library")
