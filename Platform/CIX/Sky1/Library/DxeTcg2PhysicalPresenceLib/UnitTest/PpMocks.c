#include <assert.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/HobLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/Tpm2CommandLib.h>
#include "PpMocks.h"
#include <Library/SerialPortLib.h>

PP_FIXTURE  mPp;
UINT32      mPpiMemory[3];
UINT32      mPpHashBitmap;
EFI_GUID    gEfiTcg2PhysicalPresenceGuid = EFI_TCG2_PHYSICAL_PRESENCE_DATA_GUID;

UINTN EFIAPI
Print (
  CONST CHAR16  *Fmt,
  ...
  )
{
  return mPp.PrintFailure ? 0 : StrLen (Fmt);
}

UINTN EFIAPI
SerialPortWrite (
  UINT8  *Buffer,
  UINTN  NumberOfBytes
  )
{
  assert (NumberOfBytes < sizeof (mPp.SerialLine));
  CopyMem (mPp.SerialLine, Buffer, NumberOfBytes);
  mPp.SerialLine[NumberOfBytes] = '\0';
  ++mPp.SerialWrites;
  return NumberOfBytes;
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
  assert (CompareGuid (Guid, &gEfiTcg2PhysicalPresenceGuid));
  // The flags variable carries no authority and must never be consulted.
  assert (StrCmp (Name, TCG2_PHYSICAL_PRESENCE_VARIABLE) == 0);
  if (Attr) {
    *Attr = mPp.RequestAttr;
  }

  if (!EFI_ERROR (mPp.RequestRead)) {
    CopyMem (Data, &mPp.Request, MIN (*Size, MIN (mPp.RequestSize, sizeof (mPp.Request))));
  }

  *Size = mPp.RequestSize;
  return mPp.RequestRead;
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

  assert (StrCmp (Name, TCG2_PHYSICAL_PRESENCE_VARIABLE) == 0);
  assert (Size == sizeof (mPp.Request));
  CopyMem (&mPp.Request, Data, Size);
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
static EFI_BOOT_SERVICES                mBoot    = { .CheckEvent = Check, .CreateEvent = CreateTimer, .SetTimer = SetTimer, .WaitForEvent = WaitEvent, .CloseEvent = CloseEvent };
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
  mPpHashBitmap                  = 0xF;
  mPp.Request.PPRequest           = Request;
  mPp.Request.PPRequestParameter  = Parameter;
  mPp.RequestSize                 = sizeof (mPp.Request);
  mPp.RequestAttr                 = PP_ATTR;
  mPp.Supported                   = 0x0f;
  mPp.Active                      = 0x06;
}
