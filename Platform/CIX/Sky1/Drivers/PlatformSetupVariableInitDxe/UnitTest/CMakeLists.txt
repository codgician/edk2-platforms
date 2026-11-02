# SPDX-License-Identifier: BSD-2-Clause-Patent
# AutoGen stand-in derived from the driver INF: every GUID it declares and
# every fixed PCD, valued 1 so defaults stand apart from stored test data.
file(READ "${CMAKE_CURRENT_LIST_DIR}/../PlatformSetupVariableInitDxe.inf" SETUP_INIT_INF)
string(REGEX MATCHALL "g[A-Za-z0-9]+Guid" SETUP_INIT_GUIDS "${SETUP_INIT_INF}")
list(REMOVE_DUPLICATES SETUP_INIT_GUIDS)
string(REGEX MATCHALL "TokenSpaceGuid\\.Pcd[A-Za-z0-9_]+" SETUP_INIT_PCDS "${SETUP_INIT_INF}")
set(SETUP_INIT_AUTOGEN "#include <PiDxe.h>\n#include <Library/PcdLib.h>\n")
foreach(GUID IN LISTS SETUP_INIT_GUIDS)
  string(APPEND SETUP_INIT_AUTOGEN "extern EFI_GUID ${GUID};\n")
endforeach()
foreach(PCD IN LISTS SETUP_INIT_PCDS)
  string(REPLACE "TokenSpaceGuid." "" PCD "${PCD}")
  string(APPEND SETUP_INIT_AUTOGEN "#define _PCD_VALUE_${PCD} 1\n")
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/SetupInitAutoGen.h" "${SETUP_INIT_AUTOGEN}")

sky1_host_test(setup_var_init_test PRELUDE "${CMAKE_CURRENT_BINARY_DIR}/SetupInitAutoGen.h"
  SOURCES SetupVarInitTest.c ../PlatformSetupVariableInitDxe.c
  INCLUDES "${EDK_ROOT}/NetworkPkg/Include")
