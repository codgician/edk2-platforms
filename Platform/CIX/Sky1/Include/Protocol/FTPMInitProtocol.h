#/** @file
#
#  Copyright 2022 Cix Technology (Shanghai) Co., Ltd. All Rights Reserved.
#
#**/


#ifndef _FTPM_INIT_PROTOCOL_H_
#define _FTPM_INIT_PROTOCOL_H_

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/PcdLib.h>
#include <Library/Tpm2DeviceLib.h>
#include <Library/OpteeClientApiLib.h>
#include <Library/tee_client_api.h>
#include <Library/Tcg2PhysicalPresenceLib.h>

#include <Protocol/TrEEProtocol.h>

#include <IndustryStandard/Tpm20.h>
#include <IndustryStandard/Tpm2Acpi.h>

#define CIX_FTPM_INIT_PROTOCOL_GUID \
        {0xc999ecfb, 0x43f8, 0xeb6e, { 0x4b, 0x55, 0x10, 0x1a, 0x06, 0x01, 0xa3, 0x55 }}

typedef struct _FTPM_INIT_PROTOCOL FTPM_INIT_PROTOCOL;

typedef
EFI_STATUS
(EFIAPI* GET_FTPM_SESSION) (
  IN FTPM_INIT_PROTOCOL *this,
  OUT TEEC_Context* Context,
  OUT TEEC_Session* Session
  );

typedef
EFI_STATUS
(EFIAPI* FTPM_OPEN_SESSION) (
  IN FTPM_INIT_PROTOCOL *this
  );

/** Returns TRUE only while the firmware TA session is open and usable. **/
typedef
BOOLEAN
(EFIAPI* FTPM_GET_STATUS) (
  IN FTPM_INIT_PROTOCOL *this
  );

/**
  Send a TPM command to the fTPM TA and receive its response. The protocol
  producer owns the only OP-TEE client in firmware; consumers must not link
  their own OpteeClientApiLib instance.
  Commands must contain a complete ten-byte TPM header, match its paramSize,
  and fit the TA's 0x1000-byte limit. Success guarantees a complete response
  with a valid TPM tag and paramSize equal to the returned, capacity-bounded
  size. Errors never publish response bytes; genuine short-buffer replies
  return the required size. Fatal session errors and EBS revoke readiness.
**/
typedef
EFI_STATUS
(EFIAPI* FTPM_SUBMIT_COMMAND) (
  IN FTPM_INIT_PROTOCOL *this,
  IN UINT32             InputSize,
  IN UINT8              *Input,
  IN OUT UINT32         *OutputSize,
  OUT UINT8             *Output
  );

struct _FTPM_INIT_PROTOCOL {
    GET_FTPM_SESSION     GetFTPMSession;
    FTPM_OPEN_SESSION    FTPMOpenSession;
    FTPM_GET_STATUS      GetFTPMStatus;
    FTPM_SUBMIT_COMMAND  SubmitCommand;
};

extern EFI_GUID  gCixFtpmInitProtocolGuid;
#endif
