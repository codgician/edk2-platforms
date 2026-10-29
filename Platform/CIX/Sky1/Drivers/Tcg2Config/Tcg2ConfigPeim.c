/** @file
  The module entry point for Tcg2 configuration module.

Copyright (c) 2015 - 2018, Intel Corporation. All rights reserved.<BR>
SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <PiPei.h>

#include <Guid/TpmInstance.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PeiServicesLib.h>
#include <Library/PcdLib.h>

#include <Ppi/ReadOnlyVariable2.h>
#include <Ppi/TpmInitialized.h>
#include <Protocol/Tcg2Protocol.h>

#include "Tcg2ConfigNvData.h"
#include "Tcg2Internal.h"


CONST EFI_PEI_PPI_DESCRIPTOR  gTpmSelectedPpi = {
  (EFI_PEI_PPI_DESCRIPTOR_PPI | EFI_PEI_PPI_DESCRIPTOR_TERMINATE_LIST),
  &gEfiTpmDeviceSelectedGuid,
  NULL
};



/**
  The entry point for Tcg2 configuration driver.

  @param  FileHandle  Handle of the file being invoked.
  @param  PeiServices Describes the list of possible PEI Services.

  @retval EFI_SUCCESS            Convert variable to PCD successfully.
  @retval Others                 Fail to convert variable to PCD.
**/
EFI_STATUS
EFIAPI
Tcg2ConfigPeimEntryPoint (
  IN       EFI_PEI_FILE_HANDLE  FileHandle,
  IN CONST EFI_PEI_SERVICES     **PeiServices
  )
{
  EFI_STATUS  Status;
  UINTN       Size;
  EFI_GUID    TpmInstanceGuid = TPM_DEVICE_INTERFACE_TPM20_DTPM;

  // Sky1's fTPM backend uses the TPM2 instance GUID. Neither unauthenticated
  // setup variables nor cached S3 detection may select TPM1.2 or no TPM.
  Size = sizeof (TpmInstanceGuid);
  Status = PcdSetPtrS (PcdTpmInstanceGuid, &Size, &TpmInstanceGuid);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = PeiServicesInstallPpi (&gTpmSelectedPpi);

  return Status;
}
