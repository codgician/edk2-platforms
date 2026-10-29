/** @file
  The OP-TEE Share Memory component is used to manage memory allocations
  from the memory block used for parameters & buffers shared between the
  client in normal world and the OpTEE OS in secure world.

  Copyright (c) Microsoft Corporation. All rights reserved.

  This program and the accompanying materials are licensed and made available under
  the terms and conditions of the BSD License which accompanies this distribution.
  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
**/

#ifndef __OPTEE_CLIENT_API_LIB_H__
#define __OPTEE_CLIENT_API_LIB_H__

#include <Uefi.h>
#include <Library/tee_client_api.h>

/**
  Close a session, retaining its state unless OP-TEE confirms the close.

  @param[in,out] Session  Open session to close.
  @retval TEEC_SUCCESS  OP-TEE confirmed closure and Session was cleared.
  @retval TEEC_ERROR_BAD_PARAMETERS  Session is not usable.
  @return OP-TEE's close result or the transport/allocation error.
**/
TEEC_Result
OpteeClientCloseSession (
  IN TEEC_Session  *Session
  );

EFI_STATUS
OpteeClientApiInitialize (
  IN EFI_HANDLE  ImageHandle
  );

/**
  Scrub/release all normal-world client storage and drain cached RPC arguments.

  This releases pool allocations and must run while pool services are legal,
  not after ExitBootServices. It does not itself close TA sessions.

  @retval TRUE   All local storage was released; RPC cache drain succeeded.
  @retval FALSE  A call is active or secure-world cache drain failed. A failed
                 drain poisons the client: only a reboot may resume its use.
**/
BOOLEAN
OpteeClientApiFinalize (
  VOID
  );

#endif // __OPTEE_CLIENT_API_LIB_H__
