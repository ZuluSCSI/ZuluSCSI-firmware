/**
 * Copyright (C) 2025-2026 Kevin Moonlight <me@yyzkevin.com>
 *
 * This file is part of BlueSCSI
 *
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

// Custom SCSI inquiry data (VPD/SPD) from INI configuration
// Used by AS/400 systems that require specific inquiry responses.

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <scsi2sd.h>
#include <ZuluSCSI_platform_config.h>

#ifdef __cplusplus
extern "C" {
#endif


// Clear all per-target custom inquiry/VPD/MODE SENSE state. Call once before
// the per-SCSI-ID scan loop that calls parseCustomInquiryData() below -- the
// per-ID call itself must NOT reset shared storage, or each ID's call wipes
// every previously-processed ID's custom data.
void resetCustomInquiryData();

// Parse custom inquiry data from zuluscsi.ini for one SCSI ID.
// Called once per discovered SCSI ID during initialization, after
// resetCustomInquiryData() has been called once for the whole scan.
// INI format: [SCSI<id>] vpd00=XX XX XX, spd=XX XX XX (hex values)
void parseCustomInquiryData(uint8_t scsiId, S2S_CFG_TYPE type);

// Check if custom VPD (Vital Product Data) exists for a given SCSI ID and page code.
// If found, copies data into buf and sets *length. Returns true if custom data exists.
bool getCustomVPD(uint8_t scsiId, uint8_t pageCode, uint8_t *buf, uint8_t *length);

// Check if custom SPD (Standard Page Data / standard inquiry override) exists for a SCSI ID.
// If found, copies data into buf and sets *length. Returns true if custom data exists.
bool getCustomSPD(uint8_t scsiId, uint8_t *buf, uint16_t *length);

// Check if a custom MODE SENSE page 0x3F (all pages) response exists for a SCSI ID.
// If found, copies data into buf and sets *length. Returns true if custom data exists.
bool getCustomModeSense(uint8_t scsiId, uint8_t *buf, uint16_t *length);

#ifdef PLATFORM_AS400
// Check if a loaded AS/400 disk profile (see AS400_DiskProfile=, parsed by
// parseCustomInquiryData() above) supplied a capacity for this SCSI ID. If
// so, fills *blockSize/*sectors and returns true -- used to auto-create a
// correctly-sized image file when none exists yet for a profiled ID.
bool getAS400ProfileCapacity(uint8_t scsiId, uint32_t *blockSize, uint32_t *sectors);
#endif

#ifdef __cplusplus
}
#endif
