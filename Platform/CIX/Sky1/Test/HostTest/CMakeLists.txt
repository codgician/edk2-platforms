# Native unit tests and fuzzers for Sky1 firmware code.
#
# Production sources are compiled for the build host with ASan/UBSan; every
# UnitTest/CMakeLists.txt below Platform/CIX/Sky1 (edk2-platforms) and below
# SecurityPkg (edk2) is added automatically. From the workspace root (the
# directory holding edk2 and edk2-platforms):
#
#   nix-shell edk2-platforms/Platform/CIX/Sky1/Test/HostTest/shell.nix --run '
#     cmake -S edk2-platforms/Platform/CIX/Sky1/Test/HostTest -B Build/HostTest -G Ninja &&
#     cmake --build Build/HostTest &&
#     ctest --test-dir Build/HostTest --output-on-failure'
#
# In a UnitTest/CMakeLists.txt:
#   sky1_host_test(<name> SOURCES <test and production .c files>)
#     builds a cmocka executable and registers it with ctest.
#   sky1_host_fuzz(<name> SOURCES <fuzz target and production .c files>)
#     builds a libFuzzer executable; ctest runs a short deterministic smoke.
# Both accept INCLUDES <dirs> and DEFINES <macros>.

cmake_minimum_required(VERSION 3.20)
project(Sky1HostTest C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

get_filename_component(PLAT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
get_filename_component(EDK_ROOT "${PLAT_ROOT}/../edk2" ABSOLUTE)
set(EDK_ROOT "${EDK_ROOT}" CACHE PATH "edk2 source tree")
set(SKY1_ROOT "${PLAT_ROOT}/Platform/CIX/Sky1")

if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "Use clang (shell.nix); the fuzzers need libFuzzer.")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(CMOCKA REQUIRED IMPORTED_TARGET cmocka)

set(SKY1_SANITIZE -fsanitize=address,undefined -fno-sanitize-recover=all)

# Headers, ABI and warnings shared by production and test translation units.
add_library(sky1_host_flags INTERFACE)
target_include_directories(sky1_host_flags INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/Common"
  "${EDK_ROOT}/MdePkg/Include"
  "${EDK_ROOT}/MdePkg/Include/AArch64"
  "${EDK_ROOT}/ArmPkg/Include"
  "${EDK_ROOT}/MdeModulePkg/Include"
  "${EDK_ROOT}/SecurityPkg/Include"
  "${EDK_ROOT}/CryptoPkg/Include"
  "${SKY1_ROOT}/Include"
  "${PLAT_ROOT}/Silicon/CIX/Sky1/Include")
target_compile_options(sky1_host_flags INTERFACE
  -include HostAutoGen.h
  -fshort-wchar -fno-strict-aliasing -fno-omit-frame-pointer -g -O1
  -Wall -Wno-unused-function -Wno-macro-redefined
  ${SKY1_SANITIZE})
target_link_options(sky1_host_flags INTERFACE ${SKY1_SANITIZE})

# Host implementations of the EDK2 base services production code links.
add_library(sky1_host_support STATIC Common/HostSupport.c)
target_link_libraries(sky1_host_support PUBLIC sky1_host_flags)

enable_testing()

function(sky1_host_test NAME)
  cmake_parse_arguments(ARG "" "" "SOURCES;INCLUDES;DEFINES" ${ARGN})
  add_executable(${NAME} ${ARG_SOURCES})
  target_include_directories(${NAME} PRIVATE ${ARG_INCLUDES})
  target_compile_definitions(${NAME} PRIVATE ${ARG_DEFINES})
  target_link_libraries(${NAME} PRIVATE sky1_host_support PkgConfig::CMOCKA)
  add_test(NAME ${NAME} COMMAND ${NAME})
  set_tests_properties(${NAME} PROPERTIES
    ENVIRONMENT "ASAN_OPTIONS=detect_leaks=1:abort_on_error=1;UBSAN_OPTIONS=print_stacktrace=1")
endfunction()

function(sky1_host_fuzz NAME)
  cmake_parse_arguments(ARG "" "RUNS" "SOURCES;INCLUDES;DEFINES" ${ARGN})
  if(NOT ARG_RUNS)
    set(ARG_RUNS 20000)
  endif()
  add_executable(${NAME} ${ARG_SOURCES})
  target_include_directories(${NAME} PRIVATE ${ARG_INCLUDES})
  target_compile_definitions(${NAME} PRIVATE ${ARG_DEFINES})
  target_compile_options(${NAME} PRIVATE -fsanitize=fuzzer-no-link)
  target_link_options(${NAME} PRIVATE -fsanitize=fuzzer)
  target_link_libraries(${NAME} PRIVATE sky1_host_support)
  add_test(NAME ${NAME}_smoke COMMAND ${NAME} -runs=${ARG_RUNS} -seed=1)
endfunction()

file(GLOB_RECURSE SKY1_UNIT_TESTS
  "${SKY1_ROOT}/CMakeLists.txt"
  "${EDK_ROOT}/SecurityPkg/CMakeLists.txt")
list(FILTER SKY1_UNIT_TESTS INCLUDE REGEX "/UnitTest/CMakeLists\\.txt$")
foreach(LISTFILE IN LISTS SKY1_UNIT_TESTS)
  get_filename_component(DIR "${LISTFILE}" DIRECTORY)
  file(RELATIVE_PATH REL "${PLAT_ROOT}/.." "${DIR}")
  string(MAKE_C_IDENTIFIER "${REL}" BIN)
  add_subdirectory("${DIR}" "${BIN}")
endforeach()
