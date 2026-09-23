/** @file

  @copyright
  Copyright 2020 - 2021 Intel Corporation. <BR>
  Copyright (c) 2021 - 2022, American Megatrends International LLC. <BR>
  Copyright 2025 Radxa Computer (Shenzhen) Co., Ltd. All Rights Reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef _PLATFORM_INFO_TYPES_H_
#define _PLATFORM_INFO_TYPES_H_

#ifndef VFRCOMPILE
//
// Platform types - used with EFI_PLATFORM_INFO BoardId
//
typedef enum
{
  StartOfEfiPlatformTypeEnum = 0x00,
  // For PPO
  TypeRadxaOrionO6,
  TypeRadxaOrionO6N,
  TypeRadxaOrionO6T,
  EndOfEfiPlatformTypeEnum,
  //
  // Vendor board range currently starts at 0x80
  //
  TypeBoardPortTemplate = 0x81
} EFI_PLATFORM_TYPE;
#else
//
// VfrCompile cannot parse enum (EDK II VFR spec 2.3), so mirror the
// enumerator values as macros for .vfr/.hfr consumers. Keep in sync with
// the enum above.
//
#define StartOfEfiPlatformTypeEnum  0x00
#define TypeRadxaOrionO6            0x01
#define TypeRadxaOrionO6N           0x02
#define TypeRadxaOrionO6T           0x03
#define EndOfEfiPlatformTypeEnum    0x04
#define TypeBoardPortTemplate       0x81
#endif // VFRCOMPILE

#define TypePlatformUnknown       0xFF
#define TypePlatformMin           StartOfEfiPlatformTypeEnum + 1
#define TypePlatformMax           EndOfEfiPlatformTypeEnum - 1
#define TypePlatformDefault       TypeRadxaOrionO6
#define TypePlatformVendorMin     0x80
#define TypePlatformVendorMax     TypeBoardPortTemplate - 1

#endif // #ifndef _PLATFORM_INFO_TYPES_H_
