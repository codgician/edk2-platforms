#include <assert.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/HobLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/Tpm2CommandLib.h>
#include "PpMocks.h"

PP_FIXTURE                             mPp;
UINT32                                 mPpiMemory[3];
EFI_GUID                               gEfiTcg2PhysicalPresenceGuid     = EFI_TCG2_PHYSICAL_PRESENCE_DATA_GUID;
EFI_GUID                               gEdkiiVariablePolicyProtocolGuid = EDKII_VARIABLE_POLICY_PROTOCOL_GUID;
static EDKII_VARIABLE_POLICY_PROTOCOL  mPolicy;

VOID *EFIAPI __real_AllocatePool (UINTN Size);
VOID *EFIAPI
__wrap_AllocatePool (UINTN Size)
{
  return mPp.AllocationFailure ? NULL : __real_AllocatePool (Size);
}

UINTN EFIAPI
StrLen (
  CONST CHAR16  *S
  )
{
  UINTN  N = 0;

  while (S[N]) {
    ++N;
  }

  return N;
}

UINTN EFIAPI
StrSize (
  CONST CHAR16  *S
  )
{
  return (StrLen (S) + 1) * 2;
}

INTN EFIAPI
StrCmp (
  CONST CHAR16  *A,
  CONST CHAR16  *B
  )
{
  while (*A && *A == *B) {
    ++A;
    ++B;
  }

  return *A - *B;
}

UINTN EFIAPI
Print (
  CONST CHAR16  *Fmt,
  ...
  )
{
  return mPp.PrintFailure ? 0 : StrLen (Fmt);
}

EFI_BOOT_MODE EFIAPI
GetBootModeHob (
  VOID
  )
{
  return BOOT_WITH_FULL_CONFIGURATION;
}

static EFI_STATUS EFIAPI
GetVariable (
  CHAR16    *Name,
  EFI_GUID  *Guid,
  UINT32    *Attr,
  UINTN     *Size,
  VOID      *Data
  )
{
  BOOLEAN  Flags = StrCmp (Name, TCG2_PHYSICAL_PRESENCE_FLAGS_VARIABLE) == 0;

  assert (CompareGuid (Guid, &gEfiTcg2PhysicalPresenceGuid));
  assert (Flags || StrCmp (Name, TCG2_PHYSICAL_PRESENCE_VARIABLE) == 0);
  UINTN       N      = Flags ? mPp.FlagsSize : mPp.RequestSize;
  EFI_STATUS  Status = Flags ? mPp.FlagsRead : mPp.RequestRead;
  if (Attr) {
    *Attr = Flags ? mPp.FlagsAttr : mPp.RequestAttr;
  }

  if (!EFI_ERROR (Status)) {
    CopyMem (Data, Flags ? (VOID *)&mPp.Flags : (VOID *)&mPp.Request, MIN (*Size, MIN (N, Flags ? sizeof (mPp.Flags) : sizeof (mPp.Request))));
  }

  *Size = N;
  return Status;
}

static EFI_STATUS EFIAPI
SetVariable (
  CHAR16    *Name,
  EFI_GUID  *Guid,
  UINT32    Attr,
  UINTN     Size,
  VOID      *Data
  )
{
  assert (CompareGuid (Guid, &gEfiTcg2PhysicalPresenceGuid));
  assert (Attr == PP_ATTR);
  ++mPp.Writes;
  if (mPp.FailWriteNumber == mPp.Writes) {
    return EFI_DEVICE_ERROR;
  }

  if (EFI_ERROR (mPp.WriteStatus)) {
    return mPp.WriteStatus;
  }

  if (StrCmp (Name, TCG2_PHYSICAL_PRESENCE_FLAGS_VARIABLE) == 0) {
    assert (Size == sizeof (mPp.Flags));
    if (mPp.Locked) {
      return EFI_WRITE_PROTECTED;
    }

    CopyMem (&mPp.Flags, Data, Size);
    mPp.FlagsRead = EFI_SUCCESS;
  } else {
    assert (StrCmp (Name, TCG2_PHYSICAL_PRESENCE_VARIABLE) == 0);
    assert (Size == sizeof (mPp.Request));
    CopyMem (&mPp.Request, Data, Size);
  }

  return EFI_SUCCESS;
}

static VOID EFIAPI
ResetSystem (
  EFI_RESET_TYPE  Type,
  EFI_STATUS      Status,
  UINTN           Size,
  VOID            *Data
  )
{
  ++mPp.Resets;
}

static EFI_STATUS EFIAPI
Enabled (
  BOOLEAN  *State
  )
{
  *State = mPp.PolicyEnabled;
  return mPp.PolicyStatus;
}

static EFI_STATUS EFIAPI
RegisterPolicy (
  CONST VARIABLE_POLICY_ENTRY  *Entry
  )
{
  assert (Entry->MinSize == sizeof (mPp.Flags) && Entry->MaxSize == sizeof (mPp.Flags));
  assert (Entry->AttributesMustHave == PP_ATTR && Entry->AttributesCantHave == (UINT32) ~PP_ATTR);
  assert (Entry->LockPolicyType == VARIABLE_POLICY_TYPE_LOCK_NOW);
  if (!EFI_ERROR (mPp.LockStatus)) {
    mPp.Locked = TRUE;
  }

  return mPp.LockStatus;
}

static EFI_STATUS EFIAPI
DumpPolicy (
  UINT8   *Buffer,
  UINT32  *Size
  )
{
  struct {
    VARIABLE_POLICY_ENTRY    Entry;
    CHAR16                   Name[sizeof (TCG2_PHYSICAL_PRESENCE_FLAGS_VARIABLE) / 2];
  } Policy;
  if (!mPp.Locked) {
    *Size = 0;
    return EFI_SUCCESS;
  }

  if (mPp.PolicyMismatch == 4) {
    return EFI_UNSUPPORTED;
  }

  if (*Size < sizeof (Policy)) {
    *Size = sizeof (Policy);
    return EFI_BUFFER_TOO_SMALL;
  }

  ZeroMem (&Policy, sizeof (Policy));
  Policy.Entry.Version            = VARIABLE_POLICY_ENTRY_REVISION;
  Policy.Entry.Size               = sizeof (Policy);
  Policy.Entry.OffsetToName       = sizeof (Policy.Entry);
  Policy.Entry.Namespace          = gEfiTcg2PhysicalPresenceGuid;
  Policy.Entry.MinSize            = Policy.Entry.MaxSize = sizeof (mPp.Flags);
  Policy.Entry.AttributesMustHave = PP_ATTR;
  Policy.Entry.AttributesCantHave = (UINT32) ~PP_ATTR;
  Policy.Entry.LockPolicyType     = VARIABLE_POLICY_TYPE_LOCK_NOW;
  if (mPp.PolicyMismatch == 1) {
    Policy.Entry.MinSize++;
  }

  if (mPp.PolicyMismatch == 2) {
    Policy.Entry.AttributesMustHave = 0;
  }

  if (mPp.PolicyMismatch == 3) {
    Policy.Entry.LockPolicyType = VARIABLE_POLICY_TYPE_NO_LOCK;
  }

  if (mPp.PolicyMismatch == 5) {
    Policy.Entry.Size = 0;
  }

  if (mPp.PolicyMismatch == 6) {
    Policy.Entry.OffsetToName = 0xffff;
  }

  if (mPp.PolicyMismatch == 7) {
    Policy.Entry.Size = sizeof (Policy) + 1;
  }

  CopyMem (Policy.Name, TCG2_PHYSICAL_PRESENCE_FLAGS_VARIABLE, sizeof (Policy.Name));
  if (mPp.PolicyMismatch == 8) {
    Policy.Name[0] = L'X';
  }

  CopyMem (Buffer, &Policy, sizeof (Policy));
  *Size = sizeof (Policy);
  if (mPp.PolicyMismatch == 9) {
    *Size = sizeof (VARIABLE_POLICY_ENTRY) - 1;
  }

  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
Locate (
  EFI_GUID  *Guid,
  VOID      *Registration,
  VOID      **Interface
  )
{
  assert (CompareGuid (Guid, &gEdkiiVariablePolicyProtocolGuid));
  if (!mPp.PolicyAvailable) {
    return EFI_NOT_FOUND;
  }

  *Interface = &mPolicy;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
Check (
  EFI_EVENT  Event
  )
{
  return mPp.Timeout ? EFI_SUCCESS : EFI_NOT_READY;
}

static EFI_STATUS EFIAPI
ReadKey (
  EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  EFI_INPUT_KEY                   *Key
  )
{
  ++mPp.KeyReads;
  Key->UnicodeChar = 0;
  Key->ScanCode    = mPp.Accept ? SCAN_F12 : SCAN_ESC;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
CreateTimer (
  UINT32            Type,
  EFI_TPL           Tpl,
  EFI_EVENT_NOTIFY  Notify,
  VOID              *Context,
  EFI_EVENT         *Event
  )
{
  *Event = (VOID *)2;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
SetTimer (
  EFI_EVENT        Event,
  EFI_TIMER_DELAY  Delay,
  UINT64           Time
  )
{
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
WaitEvent (
  UINTN      Count,
  EFI_EVENT  *Events,
  UINTN      *Index
  )
{
  *Index = mPp.Timeout ? 1 : 0;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
CloseEvent (
  EFI_EVENT  Event
  )
{
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
InputReset (
  EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  BOOLEAN                         Extended
  )
{
  return mPp.ConsoleStatus;
}

static EFI_SIMPLE_TEXT_INPUT_PROTOCOL   mInput = { .Reset = InputReset, .ReadKeyStroke = ReadKey, .WaitForKey = (VOID *)1 };
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  mOutput;
static EFI_SYSTEM_TABLE                 mSystem  = { .ConIn = &mInput, .ConOut = &mOutput };
EFI_SYSTEM_TABLE                        *gST     = &mSystem;
static EFI_RUNTIME_SERVICES             mRuntime = { .GetVariable = GetVariable, .SetVariable = SetVariable, .ResetSystem = ResetSystem };
EFI_RUNTIME_SERVICES                    *gRT     = &mRuntime;
static EFI_BOOT_SERVICES                mBoot    = { .LocateProtocol = Locate, .CheckEvent = Check, .CreateEvent = CreateTimer, .SetTimer = SetTimer, .WaitForEvent = WaitEvent, .CloseEvent = CloseEvent };
EFI_BOOT_SERVICES                       *gBS     = &mBoot;
static EFI_STATUS
Command (
  UINT32  Code
  )
{
  assert (mPp.TraceCount < 7);
  mPp.Trace[mPp.TraceCount++] = Code;
  return mPp.CommandStatus;
}

EFI_STATUS EFIAPI
Tpm2ClearControl (
  TPMI_RH_CLEAR      Handle,
  TPMS_AUTH_COMMAND  *Auth,
  TPMI_YES_NO        Disable
  )
{
  assert (Handle == TPM_RH_PLATFORM && Disable == NO);
  return Command (1);
}

EFI_STATUS EFIAPI
Tpm2Clear (
  TPMI_RH_CLEAR      Handle,
  TPMS_AUTH_COMMAND  *Auth
  )
{
  assert (Handle == TPM_RH_PLATFORM);
  return Command (2);
}

EFI_STATUS EFIAPI
Tpm2ChangeEPS (
  TPMI_RH_PLATFORM   Handle,
  TPMS_AUTH_COMMAND  *Auth
  )
{
  assert (Handle == TPM_RH_PLATFORM);
  return Command (3);
}

EFI_STATUS EFIAPI
Tpm2GetCapabilitySupportedAndActivePcrs (
  UINT32  *Supported,
  UINT32  *Active
  )
{
  ++mPp.Capabilities;
  *Supported = mPp.Supported;
  *Active    = mPp.Active;
  return mPp.CapabilityStatus;
}

EFI_STATUS EFIAPI
Tpm2PcrAllocateBanks (
  TPM2B_AUTH  *Auth,
  UINT32      Supported,
  UINT32      Banks
  )
{
  assert (Supported == mPp.Supported);
  mPp.Trace[7] = Banks;
  return Command (4);
}

VOID
PpReset (
  UINT8   Request,
  UINT32  Parameter
  )
{
  ZeroMem (&mPp, sizeof (mPp));
  ZeroMem (mPpiMemory, sizeof (mPpiMemory));
  mPp.Request.PPRequest           = Request;
  mPp.Request.PPRequestParameter  = Parameter;
  mPp.Flags.PPFlags               = 0x700e2;
  mPp.RequestSize                 = sizeof (mPp.Request);
  mPp.FlagsSize                   = sizeof (mPp.Flags);
  mPp.RequestAttr                 = mPp.FlagsAttr = PP_ATTR;
  mPp.PolicyAvailable             = mPp.PolicyEnabled = mPp.Locked = TRUE;
  mPp.Supported                   = 0x0f;
  mPp.Active                      = 0x06;
  mPolicy.Revision                = 0x10000;
  mPolicy.IsVariablePolicyEnabled = Enabled;
  mPolicy.RegisterVariablePolicy  = RegisterPolicy;
  mPolicy.DumpVariablePolicy      = DumpPolicy;
}
