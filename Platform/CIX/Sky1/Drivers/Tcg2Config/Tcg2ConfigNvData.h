/** @file
  Header file for NV data structure definition.

Copyright (c) 2015 - 2018, Intel Corporation. All rights reserved.<BR>
SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef __TCG2_CONFIG_NV_DATA_H__
#define __TCG2_CONFIG_NV_DATA_H__

#include <Uefi/UefiMultiPhase.h>
#include <Guid/HiiPlatformSetupFormset.h>
#include <Guid/Tcg2ConfigHii.h>
#include <IndustryStandard/TcgPhysicalPresence.h>

#define TCG2_CONFIGURATION_INFO_VARSTORE_ID  0x0002
#define TCG2_CONFIGURATION_FORM_ID           0x0001

#define KEY_TPM_DEVICE                0x2000
#define KEY_TPM2_OPERATION            0x2001
#define KEY_TPM2_OPERATION_PARAMETER  0x2002
#define KEY_TPM2_PCR_BANKS_REQUEST_0  0x2003
#define KEY_TPM2_PCR_BANKS_REQUEST_1  0x2004
#define KEY_TPM2_PCR_BANKS_REQUEST_2  0x2005
#define KEY_TPM2_PCR_BANKS_REQUEST_3  0x2006
#define KEY_TPM2_PCR_BANKS_REQUEST_4  0x2007
#define KEY_TPM_DEVICE_INTERFACE      0x2008
#define KEY_TCG2_PPI_VERSION          0x2009
#define KEY_TPM2_ACPI_REVISION        0x200A

#define TPM_DEVICE_NULL      0
#define TPM_DEVICE_1_2       1
#define TPM_DEVICE_2_0_DTPM  2


#define TCG2_PPI_VERSION_1_2  0x322E31                    // "1.2"
#define TCG2_PPI_VERSION_1_3  0x332E31                    // "1.3"


typedef struct {
  BOOLEAN    Sha1Supported;
  BOOLEAN    Sha256Supported;
  BOOLEAN    Sha384Supported;
  BOOLEAN    Sha512Supported;
  BOOLEAN    Sm3Supported;
  BOOLEAN    ChangeEPSSupported;
} TCG2_CONFIGURATION_INFO;

#define TCG2_STORAGE_INFO_NAME      L"TCG2_CONFIGURATION_INFO"

#endif
