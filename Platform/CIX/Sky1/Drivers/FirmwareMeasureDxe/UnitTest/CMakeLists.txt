# SPDX-License-Identifier: BSD-2-Clause-Patent
# The real event-record and HOB libraries produce the logged events.
sky1_host_test(firmware_measure_test
  SOURCES FirmwareMeasureTest.c
    "${EDK_ROOT}/SecurityPkg/Library/TcgEventLogRecordLib/TcgEventLogRecordLib.c"
    "${EDK_ROOT}/MdePkg/Library/DxeHobLib/HobLib.c"
    "${EDK_ROOT}/MdePkg/Library/BasePrintLib/PrintLib.c"
    "${EDK_ROOT}/MdePkg/Library/BasePrintLib/PrintLibInternal.c"
    "${EDK_ROOT}/MdePkg/Library/BaseLib/DivU64x32Remainder.c"
    "${EDK_ROOT}/MdePkg/Library/BaseLib/Math64.c"
    "${EDK_ROOT}/MdePkg/Library/BaseLib/Unaligned.c")
target_compile_options(firmware_measure_test PRIVATE
  "SHELL:-include ${CMAKE_CURRENT_LIST_DIR}/TestPcd.h"
  -ffunction-sections -fdata-sections)
target_link_options(firmware_measure_test PRIVATE -Wl,--gc-sections)
