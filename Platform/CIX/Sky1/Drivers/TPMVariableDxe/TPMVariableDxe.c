/** @file
  Initialize the PP flags once and require an exact, immediately locked policy.

  Copyright 2024 Cix Technology Group Co., Ltd. All Rights Reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Uefi.h>
#include <Library/PcdLib.h>
#include "../../Library/DxeTcg2PhysicalPresenceLib/PpPolicy.h"

/** Initialize flags only on NOT_FOUND, then register and verify LOCK_NOW.

  Optional flag-changing PPI operations are deliberately unsupported: no trusted
  write window or reset-spanning authorization token is needed.

  @param[in] ImageHandle  Driver image handle.
  @param[in] SystemTable UEFI system table.

  @retval EFI_SUCCESS  Exact flags and their lock have been verified.
  @retval Others       Do not process physical-presence requests this boot.
**/
EFI_STATUS
EFIAPI
TPMVariableDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                        Status;
  EFI_TCG2_PHYSICAL_PRESENCE_FLAGS  Flags;
  EDKII_VARIABLE_POLICY_PROTOCOL    *Policy;
  BOOLEAN                           Enabled;

  struct {
    VARIABLE_POLICY_ENTRY    Entry;
    CHAR16                   Name[sizeof (TCG2_PHYSICAL_PRESENCE_FLAGS_VARIABLE) / sizeof (CHAR16)];
  } Lock;

  Status = PpReadFlags (&Flags);
  if (Status == EFI_NOT_FOUND) {
    Flags.PPFlags = PcdGet32 (PcdTcg2PhysicalPresenceFlags) | PP_REQUIRED_FLAGS;
    if ((Flags.PPFlags & ~PP_FLAGS_MASK) != 0) {
      return PpUnavailable ("initial flags schema invalid", EFI_COMPROMISED_DATA);
    }

    Status = gRT->SetVariable (
                               TCG2_PHYSICAL_PRESENCE_FLAGS_VARIABLE,
                               &gEfiTcg2PhysicalPresenceGuid,
                               PP_VARIABLE_ATTRIBUTES,
                               sizeof (Flags),
                               &Flags
                               );
    if (!EFI_ERROR (Status)) {
      Status = PpReadFlags (&Flags);
    }
  }

  if (EFI_ERROR (Status)) {
    return PpUnavailable ("flags initialization or schema failed", Status);
  }

  Status = gBS->LocateProtocol (&gEdkiiVariablePolicyProtocolGuid, NULL, (VOID **)&Policy);
  if (EFI_ERROR (Status)) {
    return PpUnavailable ("flags lock protocol unavailable", Status);
  }

  if ((Policy->Revision < 0x0000000000010000ULL) ||
      (Policy->IsVariablePolicyEnabled == NULL) || (Policy->RegisterVariablePolicy == NULL))
  {
    return PpUnavailable ("flags lock registration unsupported", EFI_UNSUPPORTED);
  }

  Enabled = FALSE;
  Status  = Policy->IsVariablePolicyEnabled (&Enabled);
  if (EFI_ERROR (Status) || !Enabled) {
    return PpUnavailable ("variable policy disabled or unavailable", EFI_SECURITY_VIOLATION);
  }

  ZeroMem (&Lock, sizeof (Lock));
  Lock.Entry.Version            = VARIABLE_POLICY_ENTRY_REVISION;
  Lock.Entry.Size               = sizeof (Lock);
  Lock.Entry.OffsetToName       = sizeof (VARIABLE_POLICY_ENTRY);
  Lock.Entry.Namespace          = gEfiTcg2PhysicalPresenceGuid;
  Lock.Entry.MinSize            = sizeof (Flags);
  Lock.Entry.MaxSize            = sizeof (Flags);
  Lock.Entry.AttributesMustHave = PP_VARIABLE_ATTRIBUTES;
  Lock.Entry.AttributesCantHave = (UINT32) ~PP_VARIABLE_ATTRIBUTES;
  Lock.Entry.LockPolicyType     = VARIABLE_POLICY_TYPE_LOCK_NOW;
  CopyMem (Lock.Name, TCG2_PHYSICAL_PRESENCE_FLAGS_VARIABLE, sizeof (Lock.Name));
  Status = Policy->RegisterVariablePolicy (&Lock.Entry);
  // ALREADY_STARTED is not sufficient: verify that the existing policy is exact.
  if (EFI_ERROR (Status) && (Status != EFI_ALREADY_STARTED)) {
    return PpUnavailable ("flags lock registration failed", Status);
  }

  Status = PpVerifyFlagsLock ();
  // Verification reports one precise reason if the advertised lock is unusable.
  return Status;
}
