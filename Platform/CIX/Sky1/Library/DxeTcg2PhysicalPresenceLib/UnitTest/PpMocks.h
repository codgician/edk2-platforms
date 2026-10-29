#ifndef PP_MOCKS_H_
#define PP_MOCKS_H_
#include <PiDxe.h>
#include <Guid/Tcg2PhysicalPresenceData.h>
#include <Library/Tcg2PhysicalPresenceLib.h>
#include <Protocol/VariablePolicy.h>
#define PP_ATTR  (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS)
typedef struct {
  EFI_TCG2_PHYSICAL_PRESENCE          Request;
  EFI_TCG2_PHYSICAL_PRESENCE_FLAGS    Flags;
  UINTN                               RequestSize;
  UINTN                               FlagsSize;
  UINT32                              RequestAttr;
  UINT32                              FlagsAttr;
  EFI_STATUS                          RequestRead;
  EFI_STATUS                          FlagsRead;
  EFI_STATUS                          WriteStatus;
  EFI_STATUS                          PolicyStatus;
  EFI_STATUS                          LockStatus;
  EFI_STATUS                          CapabilityStatus;
  EFI_STATUS                          CommandStatus;
  EFI_STATUS                          ConsoleStatus;
  BOOLEAN                             PrintFailure;
  UINT32                              PolicyMismatch;
  UINTN                               FailWriteNumber;
  BOOLEAN                             AllocationFailure;
  BOOLEAN                             PolicyAvailable;
  BOOLEAN                             PolicyEnabled;
  BOOLEAN                             Locked;
  BOOLEAN                             Accept;
  BOOLEAN                             Timeout;
  UINT32                              Supported;
  UINT32                              Active;
  UINT32                              Trace[8];
  UINTN                               TraceCount;
  UINTN                               Writes;
  UINTN                               KeyReads;
  UINTN                               Resets;
  UINTN                               Capabilities;
} PP_FIXTURE;
extern PP_FIXTURE  mPp;
VOID
PpReset (
  UINT8   Request,
  UINT32  Parameter
  );

EFI_STATUS EFIAPI
TPMVariableDxeEntryPoint (
  EFI_HANDLE        Image,
  EFI_SYSTEM_TABLE  *SystemTable
  );

#endif
