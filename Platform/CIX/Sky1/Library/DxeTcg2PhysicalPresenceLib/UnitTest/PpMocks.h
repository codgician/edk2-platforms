#ifndef PP_MOCKS_H_
#define PP_MOCKS_H_
#include <PiDxe.h>
#include <Guid/Tcg2PhysicalPresenceData.h>
#include <Library/Tcg2PhysicalPresenceLib.h>
#define PP_ATTR  (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS)
typedef struct {
  EFI_TCG2_PHYSICAL_PRESENCE    Request;
  UINTN                         RequestSize;
  UINT32                        RequestAttr;
  EFI_STATUS                    RequestRead;
  EFI_STATUS                    WriteStatus;
  EFI_STATUS                    CapabilityStatus;
  EFI_STATUS                    CommandStatus;
  EFI_STATUS                    ConsoleStatus;
  BOOLEAN                       PrintFailure;
  UINTN                         FailWriteNumber;
  BOOLEAN                       Accept;
  BOOLEAN                       Timeout;
  UINT32                        Supported;
  UINT32                        Active;
  UINT32                        Trace[8];
  UINTN                         TraceCount;
  UINTN                         Writes;
  UINTN                         KeyReads;
  UINTN                         Resets;
  UINTN                         SerialWrites;
  CHAR8                         SerialLine[256];
} PP_FIXTURE;
extern PP_FIXTURE  mPp;
VOID
PpReset (
  UINT8   Request,
  UINT32  Parameter
  );

#endif
