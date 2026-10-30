/** @file Host PCD values for the boot gate. SPDX-License-Identifier: BSD-2-Clause-Patent **/
#define _PCD_VALUE_PcdUartDefaultBaudRate 115200ULL
#define _PCD_VALUE_PcdUartDefaultDataBits 8
#define _PCD_VALUE_PcdUartDefaultParity 1
#define _PCD_VALUE_PcdUartDefaultStopBits 1
#define _PCD_VALUE_PcdDefaultTerminalType 4
#define _PCD_VALUE_PcdRegisterFastBootSupport 0
#define _PCD_VALUE_PcdRegisterBootShellSupport 0
#define _PCD_VALUE_PcdLinuxBootSelectSupport 0
#define _PCD_GET_MODE_BOOL_PcdAndroidBoot TRUE
#define _PCD_GET_MODE_BOOL_PcdPlatformRecoverySupport FALSE
#define _PCD_GET_MODE_32_PcdBootDiscoveryPolicy 0
#define _PCD_SET_MODE_32_S_PcdBootDiscoveryPolicy(Value) EFI_SUCCESS
#define _PCD_GET_MODE_PTR_PcdFirmwareVersionString L"test"
#define _PCD_GET_MODE_16_PcdPlatformBootTimeOut 0
#define _PCD_GET_MODE_32_PcdMaximumUnicodeStringLength 0
#define _PCD_GET_MODE_32_PcdMaximumAsciiStringLength 0
