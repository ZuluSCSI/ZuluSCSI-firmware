/**
 * ZuluSCSI™ - Copyright (c) 2025 Rabbit Hole Computing™
 * Copyright (c) 2025 Kevin Moonlight <me@yyzkevin.com>
 * ZuluSCSI™ firmware is licensed under the GPL version 3 or any later version.
 *
 * https://www.gnu.org/licenses/gpl-3.0.html
 * ----
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
**/

// Real AS/400 tape drive identity captures, mirroring as400_values.h's shape
// for disk. Two variants, selected by the same Device=AS400_CISC/AS400_PPC
// preset already used for disk block-size defaults -- see
// ZuluSCSI_settings.cpp's DEV_PRESET_AS400_CISC/_PPC handling and
// custom_vendor_inquiry.cpp's loadAS400TapeDefaults().

#pragma once

#include <ZuluSCSI_platform_config.h>
#ifdef PLATFORM_AS400
#include <stdint.h>
#include <stddef.h>

// CISC-era ("IBM 4100", Tandberg-manufactured): standard INQUIRY and
// MODE SENSE(6) page 0x3F only -- the real drive has no VPD/EVPD or LOG
// SENSE support at all (confirmed: identical rejection with and without a
// tape loaded), consistent with predating those SCSI conventions.
extern const uint8_t AS400TapeCISCVendorInquiry[];
extern const size_t  AS400TapeCISCVendorInquiryLen;

extern const uint8_t as400_tape_cisc_mode_sense_all_pages[];
extern const size_t  as400_tape_cisc_mode_sense_all_pagesLen;

// PPC-era ("IBM SLR5", Tandberg-manufactured): standard INQUIRY,
// MODE SENSE(6) page 0x3F, and a 14-page VPD table (the real drive supports
// VPD/EVPD, including vendor pages 0xC7/0xD0 that real RISC-side AS/400
// traffic was independently observed requesting from a tape unit).
extern const uint8_t AS400TapePPCVendorInquiry[];
extern const size_t  AS400TapePPCVendorInquiryLen;

extern const uint8_t as400_tape_ppc_mode_sense_all_pages[];
extern const size_t  as400_tape_ppc_mode_sense_all_pagesLen;

// Same row shape as the disk table AS400VitalPages: row[0] = captured page
// length, row[1..] = the page's own bytes (4-byte SCSI VPD header
// included), so the page code is readable at row[2].
extern const uint8_t AS400TapePPCVitalPages[][255];
extern const size_t  AS400TapePPCVitalPagesLen;

#endif
