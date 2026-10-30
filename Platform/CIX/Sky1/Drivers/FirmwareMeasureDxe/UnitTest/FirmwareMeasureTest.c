/** @file
  PCR0 firmware-volume measurement: production driver, real HOB walk and real
  event-record library; only the TCG2 transport and console are mocked.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <cmocka.h>

#include "../FirmwareMeasureDxe.c"

#include <Library/TpmMeasurementLib.h>
#include <IndustryStandard/UefiTcgPlatform.h>

EFI_SYSTEM_TABLE   *gST;
EFI_BOOT_SERVICES  *gBS;
EFI_GUID           gEfiHobListGuid;
extern VOID        *mHobList;

#define FV_SIZE  4096
#define MAX_LOG  4

STATIC CONST EFI_GUID  mFvMainName = {
  0x9a15aa37, 0xd555, 0x4a4e, { 0xb5, 0x41, 0x86, 0x39, 0x1f, 0xf6, 0x81, 0x64 }
};
STATIC CONST EFI_GUID  mOtherName = {
  0x1b6c3e0d, 0x52a4, 0x4d8b, { 0x9f, 0x10, 0x2e, 0x77, 0x41, 0x0c, 0x5a, 0x3d }
};

typedef struct {
  UINT32  PcrIndex;
  UINT32  EventType;
  UINT8   Event[sizeof (PLATFORM_FIRMWARE_BLOB2_STRUCT)];
  UINT32  EventSize;
  VOID    *Data;
  UINT64  DataSize;
} MEASUREMENT;

STATIC UINT64       mFd[FV_SIZE / sizeof (UINT64)];
STATIC UINT64       mFvMain[FV_SIZE / sizeof (UINT64)];
STATIC UINT64       mOther[FV_SIZE / sizeof (UINT64)];
STATIC UINT64       mHobs[64];
STATIC MEASUREMENT  mLog[MAX_LOG];
STATIC UINTN        mMeasurements;
STATIC EFI_STATUS   mMeasureStatus;
STATIC CHAR8        mSerial[512];

EFI_STATUS
EFIAPI
TpmMeasureAndLogData (
  IN UINT32  PcrIndex,
  IN UINT32  EventType,
  IN VOID    *EventLog,
  IN UINT32  LogLen,
  IN VOID    *HashData,
  IN UINT64  HashDataLen
  )
{
  MEASUREMENT  *Entry;

  assert_true (mMeasurements < MAX_LOG);
  assert_true (LogLen <= sizeof (Entry->Event));
  Entry            = &mLog[mMeasurements++];
  Entry->PcrIndex  = PcrIndex;
  Entry->EventType = EventType;
  Entry->EventSize = LogLen;
  Entry->Data      = HashData;
  Entry->DataSize  = HashDataLen;
  memcpy (Entry->Event, EventLog, LogLen);
  return mMeasureStatus;
}

UINTN
EFIAPI
SerialPortWrite (
  IN UINT8  *Buffer,
  IN UINTN  NumberOfBytes
  )
{
  size_t  Used = strlen (mSerial);

  assert_true (Used + NumberOfBytes < sizeof (mSerial));
  memcpy (mSerial + Used, Buffer, NumberOfBytes);
  mSerial[Used + NumberOfBytes] = 0;
  return NumberOfBytes;
}

UINTN
EFIAPI
Print (
  IN CONST CHAR16  *Format,
  ...
  )
{
  fail_msg ("no console exists during DXE dispatch in these tests");
  return 0;
}

EFI_STATUS
EFIAPI
EfiGetSystemConfigurationTable (
  IN  EFI_GUID  *TableGuid,
  OUT VOID      **Table
  )
{
  fail_msg ("the HOB list is installed directly");
  return EFI_NOT_FOUND;
}

/** A minimal volume whose extension header names it, as GenFv emits. **/
STATIC
EFI_PHYSICAL_ADDRESS
MakeVolume (
  OUT UINT64          *Buffer,
  IN  CONST EFI_GUID  *Name
  )
{
  EFI_FIRMWARE_VOLUME_HEADER      *Header;
  EFI_FIRMWARE_VOLUME_EXT_HEADER  *Ext;

  memset (Buffer, 0xA5, FV_SIZE);
  Header                  = (EFI_FIRMWARE_VOLUME_HEADER *)Buffer;
  Header->Signature       = EFI_FVH_SIGNATURE;
  Header->FvLength        = FV_SIZE;
  Header->HeaderLength    = sizeof (*Header);
  Header->ExtHeaderOffset = sizeof (*Header);
  Ext                     = (EFI_FIRMWARE_VOLUME_EXT_HEADER *)((UINT8 *)Buffer + Header->ExtHeaderOffset);
  Ext->FvName             = *Name;
  Ext->ExtHeaderSize      = sizeof (*Ext);
  return (EFI_PHYSICAL_ADDRESS)(UINTN)Buffer;
}

STATIC
UINT8 *
AddHob (
  IN UINT8   *Cursor,
  IN UINT16  Type,
  IN UINT16  Length
  )
{
  EFI_HOB_GENERIC_HEADER  *Header = (EFI_HOB_GENERIC_HEADER *)Cursor;

  assert_true (Cursor + Length <= (UINT8 *)mHobs + sizeof (mHobs));
  memset (Cursor, 0, Length);
  Header->HobType   = Type;
  Header->HobLength = Length;
  return Cursor;
}

/** The FD's volume HOB, then one FV2 HOB per extracted volume. **/
STATIC
VOID
InstallHobs (
  IN UINTN  ExtractedVolumes
  )
{
  UINT8                      *Cursor = (UINT8 *)mHobs;
  EFI_HOB_FIRMWARE_VOLUME    *Fd;
  EFI_HOB_FIRMWARE_VOLUME2   *Fv2;
  UINT64                     *Buffers[] = { mFvMain, mOther };
  CONST EFI_GUID             *Names[]   = { &mFvMainName, &mOtherName };
  UINTN                      Index;

  Fd              = (EFI_HOB_FIRMWARE_VOLUME *)AddHob (Cursor, EFI_HOB_TYPE_FV, sizeof (*Fd));
  Fd->BaseAddress = MakeVolume (mFd, &mOtherName);
  Fd->Length      = FV_SIZE;
  Cursor         += sizeof (*Fd);
  for (Index = 0; Index < ExtractedVolumes; Index++) {
    Fv2              = (EFI_HOB_FIRMWARE_VOLUME2 *)AddHob (Cursor, EFI_HOB_TYPE_FV2, sizeof (*Fv2));
    Fv2->BaseAddress = MakeVolume (Buffers[Index], Names[Index]);
    Fv2->Length      = FV_SIZE;
    Fv2->FvName      = *Names[Index];
    Cursor          += sizeof (*Fv2);
  }

  AddHob (Cursor, EFI_HOB_TYPE_END_OF_HOB_LIST, sizeof (EFI_HOB_GENERIC_HEADER));
  mHobList = mHobs;
}

STATIC
int
Reset (
  void  **State
  )
{
  memset (mLog, 0, sizeof (mLog));
  mMeasurements  = 0;
  mMeasureStatus = EFI_SUCCESS;
  mSerial[0]     = 0;
  gST            = NULL;
  return 0;
}

STATIC
VOID
AssertBlob2 (
  IN CONST MEASUREMENT     *Entry,
  IN CONST UINT64          *Volume,
  IN CONST CHAR8           *Description
  )
{
  CONST PLATFORM_FIRMWARE_BLOB2_STRUCT  *Blob = (CONST PLATFORM_FIRMWARE_BLOB2_STRUCT *)Entry->Event;

  assert_int_equal (Entry->PcrIndex, 0);
  assert_int_equal (Entry->EventType, EV_EFI_PLATFORM_FIRMWARE_BLOB2);
  assert_int_equal (Entry->EventSize, sizeof (*Blob));
  assert_int_equal (strcasecmp ((CONST char *)Blob->BlobDescription, Description), 0);
  assert_int_equal (Blob->BlobBase, (UINTN)Volume);
  assert_int_equal (Blob->BlobLength, FV_SIZE);
  // PCR0 receives the digest of the whole volume, as built.
  assert_ptr_equal (Entry->Data, Volume);
  assert_int_equal (Entry->DataSize, FV_SIZE);
}

STATIC
void
ExtractedVolumesOnly (
  void  **State
  )
{
  InstallHobs (2);
  assert_int_equal (FirmwareMeasureDxeEntryPoint (NULL, NULL), EFI_SUCCESS);
  // The FD stays out: PrePi runs in place there and writes its globals.
  assert_int_equal (mMeasurements, 2);
  AssertBlob2 (&mLog[0], mFvMain, "Fv(9a15aa37-d555-4a4e-b541-86391ff68164)");
  AssertBlob2 (&mLog[1], mOther, "Fv(1b6c3e0d-52a4-4d8b-9f10-2e77410c5a3d)");
  assert_string_equal (mSerial, "");
}

STATIC
void
NoExtractedVolume (
  void  **State
  )
{
  InstallHobs (0);
  assert_int_equal (FirmwareMeasureDxeEntryPoint (NULL, NULL), EFI_NOT_FOUND);
  assert_int_equal (mMeasurements, 0);
  assert_string_equal (mSerial, "TPM FirmwareMeasure: no extracted firmware volume, PCR0 not extended (Not Found)\r\n");
}

STATIC
void
MeasurementFailureStops (
  void  **State
  )
{
  // Tcg2Dxe caps PCRs 0-7 on this failure; later extends would be refused.
  InstallHobs (2);
  mMeasureStatus = EFI_DEVICE_ERROR;
  assert_int_equal (FirmwareMeasureDxeEntryPoint (NULL, NULL), EFI_DEVICE_ERROR);
  assert_int_equal (mMeasurements, 1);
  assert_string_equal (mSerial, "TPM FirmwareMeasure: firmware volume measurement failed (Device Error)\r\n");
}

int
main (
  void
  )
{
  const struct CMUnitTest  Tests[] = {
    cmocka_unit_test_setup (ExtractedVolumesOnly,    Reset),
    cmocka_unit_test_setup (NoExtractedVolume,       Reset),
    cmocka_unit_test_setup (MeasurementFailureStops, Reset),
  };

  return cmocka_run_group_tests (Tests, NULL, NULL);
}
