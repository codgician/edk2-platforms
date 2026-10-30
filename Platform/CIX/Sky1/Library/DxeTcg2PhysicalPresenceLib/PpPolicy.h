/** @file
  Exact PP flags schema and the lock required before processing OS requests.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef SKY1_PP_POLICY_H_
#define SKY1_PP_POLICY_H_

#include <Protocol/VariablePolicy.h>
#include <Guid/Tcg2PhysicalPresenceData.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include "PpStatus.h"

#define PP_VARIABLE_ATTRIBUTES  (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS)
// RESET_TRACK is not an authorization token on this platform.
#define PP_FLAGS_MASK      0x000703F2U
#define PP_REQUIRED_FLAGS  0x000300F2U

/** Read flags only if the complete stored schema is valid.

  @param[out] Flags  Exact validated flags, or zeroes on any failure.
  @return           EFI_SUCCESS or the variable/schema error.
**/
STATIC
EFI_STATUS
PpReadFlags (
  OUT EFI_TCG2_PHYSICAL_PRESENCE_FLAGS  *Flags
  )
{
  EFI_STATUS  Status;
  UINTN       Size;
  UINT32      Attributes;

  ZeroMem (Flags, sizeof (*Flags));
  Attributes = 0;
  Size       = sizeof (*Flags);
  Status     = gRT->GetVariable (
                                 TCG2_PHYSICAL_PRESENCE_FLAGS_VARIABLE,
                                 &gEfiTcg2PhysicalPresenceGuid,
                                 &Attributes,
                                 &Size,
                                 Flags
                                 );
  if (!EFI_ERROR (Status) &&
      ((Size != sizeof (*Flags)) || (Attributes != PP_VARIABLE_ATTRIBUTES) ||
       ((Flags->PPFlags & ~PP_FLAGS_MASK) != 0)))
  {
    Status = EFI_COMPROMISED_DATA;
  }

  if (EFI_ERROR (Status)) {
    ZeroMem (Flags, sizeof (*Flags));
    DEBUG ((DEBUG_ERROR, "[TPM2] Invalid PP flags: %r\n", Status));
  }

  return Status;
}

/** Verify the exact named LOCK_NOW policy, not merely protocol presence.

  This proves the variable frontend accepted the policy. Enforcement by the
  closed StandaloneMM backend still requires on-device validation.

  @retval EFI_SUCCESS  Exact LOCK_NOW policy found in a bounded MM snapshot.
  @retval Others       Policy unavailable, disabled, malformed or missing.
**/
STATIC
EFI_STATUS
PpVerifyFlagsLock (
  VOID
  )
{
  EDKII_VARIABLE_POLICY_PROTOCOL  *Policy;
  VARIABLE_POLICY_ENTRY           Entry;
  EFI_STATUS                      Status;
  BOOLEAN                         Enabled;
  BOOLEAN                         Found;
  UINT32                          Size;
  UINT32                          Capacity;
  UINTN                           Offset;
  UINT8                           *Buffer;

  Status = gBS->LocateProtocol (&gEdkiiVariablePolicyProtocolGuid, NULL, (VOID **)&Policy);
  if (EFI_ERROR (Status)) {
    return PpUnavailable ("flags lock protocol unavailable", Status);
  }

  if ((Policy->Revision < 0x0000000000010000ULL) ||
      (Policy->IsVariablePolicyEnabled == NULL) || (Policy->DumpVariablePolicy == NULL))
  {
    return PpUnavailable ("flags lock verification unsupported", EFI_UNSUPPORTED);
  }

  Enabled = FALSE;
  Status  = Policy->IsVariablePolicyEnabled (&Enabled);
  if (EFI_ERROR (Status) || !Enabled) {
    return PpUnavailable ("variable policy disabled or unavailable", EFI_SECURITY_VIOLATION);
  }

  // MM policy commands 1..5 are supported by the original CIX backend. The
  // revision-2 per-variable queries are not; do not make them a prerequisite.
  Size   = 0;
  Status = Policy->DumpVariablePolicy (NULL, &Size);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    return PpUnavailable ("policy dump unavailable", EFI_ERROR (Status) ? Status : EFI_SECURITY_VIOLATION);
  }

  if ((Size == 0) || (Size > SIZE_64KB)) {
    return PpUnavailable ("policy dump malformed", EFI_SECURITY_VIOLATION);
  }

  Buffer = AllocatePool (Size);
  if (Buffer == NULL) {
    return PpUnavailable ("policy snapshot allocation failed", EFI_OUT_OF_RESOURCES);
  }

  Capacity = Size;
  Status   = Policy->DumpVariablePolicy (Buffer, &Size);
  Found    = FALSE;
  Offset   = 0;
  if (EFI_ERROR (Status) || (Size > Capacity)) {
    FreePool (Buffer);
    return PpUnavailable ("policy dump failed or oversized", EFI_ERROR (Status) ? Status : EFI_SECURITY_VIOLATION);
  }

  while (Offset < Size) {
    if (Size - Offset < sizeof (Entry)) {
      FreePool (Buffer);
      return PpUnavailable ("policy dump malformed", EFI_SECURITY_VIOLATION);
    }

    CopyMem (&Entry, Buffer + Offset, sizeof (Entry));
    if ((Entry.Version != VARIABLE_POLICY_ENTRY_REVISION) ||
        (Entry.Size < sizeof (Entry)) || (Entry.Size > Size - Offset) ||
        (Entry.OffsetToName < sizeof (Entry)) || (Entry.OffsetToName > Entry.Size))
    {
      FreePool (Buffer);
      return PpUnavailable ("policy dump malformed", EFI_SECURITY_VIOLATION);
    }

    if (CompareGuid (&Entry.Namespace, &gEfiTcg2PhysicalPresenceGuid) &&
        (Entry.Size - Entry.OffsetToName == sizeof (TCG2_PHYSICAL_PRESENCE_FLAGS_VARIABLE)) &&
        (CompareMem (
                     Buffer + Offset + Entry.OffsetToName,
                     TCG2_PHYSICAL_PRESENCE_FLAGS_VARIABLE,
                     sizeof (TCG2_PHYSICAL_PRESENCE_FLAGS_VARIABLE)
                     ) == 0))
    {
      // An exact match outranks namespace/wildcard policies. Reject duplicates.
      if (Found || (Entry.OffsetToName != sizeof (Entry)) ||
          (Entry.MinSize != sizeof (EFI_TCG2_PHYSICAL_PRESENCE_FLAGS)) ||
          (Entry.MaxSize != sizeof (EFI_TCG2_PHYSICAL_PRESENCE_FLAGS)) ||
          (Entry.AttributesMustHave != PP_VARIABLE_ATTRIBUTES) ||
          (Entry.AttributesCantHave != (UINT32) ~PP_VARIABLE_ATTRIBUTES) ||
          (Entry.LockPolicyType != VARIABLE_POLICY_TYPE_LOCK_NOW))
      {
        break;
      }

      Found = TRUE;
    }

    Offset += Entry.Size;
  }

  FreePool (Buffer);
  if ((Offset != Size) || !Found) {
    return PpUnavailable ("flags lock unverified", EFI_SECURITY_VIOLATION);
  }

  return EFI_SUCCESS;
}

#endif
