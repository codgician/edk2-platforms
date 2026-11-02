# SPDX-License-Identifier: BSD-2-Clause-Patent
# The real event-record and HOB libraries produce the logged events.
sky1_host_test(firmware_measure_test PRELUDE TestPcd.h
  SOURCES FirmwareMeasureTest.c
    "${EDK_ROOT}/SecurityPkg/Library/TcgEventLogRecordLib/TcgEventLogRecordLib.c"
    "${EDK_ROOT}/MdePkg/Library/DxeHobLib/HobLib.c")
