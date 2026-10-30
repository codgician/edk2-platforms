#ifndef PP_HOST_AUTOGEN_H_
#define PP_HOST_AUTOGEN_H_
#include <PiDxe.h>
#include <Library/PcdLib.h>
extern UINT32  mPpiMemory[3];
extern UINT32  mPpHashBitmap;
extern UINT32  mPpHashMask;
#define _PCD_GET_MODE_32_PcdTcg2HashAlgorithmBitmap  mPpHashBitmap
#define _PCD_GET_MODE_32_PcdTpm2HashMask             mPpHashMask
#define _PCD_GET_MODE_32_PcdMaximumAsciiStringLength    0U
#define _PCD_GET_MODE_32_PcdMaximumUnicodeStringLength  0U
#define _PCD_GET_MODE_32_PcdTcg2PhysicalPresenceFlags  0x700e2U
#define _PCD_VALUE_PcdTPMPPIMemoryBase                 ((UINTN)mPpiMemory)
#endif
