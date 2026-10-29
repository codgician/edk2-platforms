#ifndef PP_HOST_AUTOGEN_H_
#define PP_HOST_AUTOGEN_H_
#include <PiDxe.h>
#include <Library/PcdLib.h>
extern UINT32  mPpiMemory[3];
#define _PCD_GET_MODE_32_PcdTcg2PhysicalPresenceFlags  0x700e2U
#define _PCD_VALUE_PcdTPMPPIMemoryBase                 ((UINTN)mPpiMemory)
#endif
