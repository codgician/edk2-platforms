# SPDX-License-Identifier: BSD-2-Clause-Patent
# The cap path must use the resident transport even when no protocol exists.
set(FTPM_DRIVER_SOURCES
  DriverHarness.c
  ../ResidentTpm2DeviceLib/ResidentTpm2DeviceLib.c
  "${EDK_ROOT}/SecurityPkg/Library/Tpm2CommandLib/Tpm2Capability.c"
  "${EDK_ROOT}/SecurityPkg/Library/Tpm2CommandLib/Tpm2Help.c"
  "${EDK_ROOT}/SecurityPkg/Library/Tpm2CommandLib/Tpm2Integrity.c")
sky1_host_test(ftpmdxe_test SOURCES DriverTest.c ${FTPM_DRIVER_SOURCES})
sky1_host_fuzz(response_fuzz SOURCES ResponseFuzz.c ${FTPM_DRIVER_SOURCES})
