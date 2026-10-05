/**
 * ZuluSCSI™ - Copyright (c) 2022-2025 Rabbit Hole Computing™
 *
 * This file is licensed under the GPL version 3 or any later version.
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

/*
 * Reads MBR and GPT partition tables directly off the SD card to resolve a
 * partition number to its (startSector, sectorCount) extent, for the
 * PART:n image-source form (see ImageBackingStore).
 *
 * This duplicates functionality that partially exists in the vendored
 * SdFat library (common/PartitionTable.cpp), which is NOT reachable
 * through SdFat's own public header (SdFat.h only pulls in
 * ExFatLib/FatLib/FsLib/SdCard, not common/PartitionTable.h), is capped at
 * partition index 4 for both MBR and GPT with no way to raise that
 * without patching SdFat's own internals, does not return partition size
 * (only start sector), and does not validate the GPT header/partition-
 * array CRC32 checksums at all -- only the "EFI PART" signature.
 *
 * The duplication is deliberate, not an oversight: MBR and GPT are
 * stable, publicly-documented, industry-standard on-disk formats, not
 * SdFat inventions, so re-implementing just what's needed here is a
 * contained, low-risk piece of code. The DESIRED long-term direction is
 * to upstream these improvements (size, GPT index >4, CRC32 validation)
 * into the rabbitholecomputing/SdFat fork itself and retire this file in
 * favor of SdFat's own (expanded) API -- this file is a stopgap, not a
 * permanent parallel implementation to maintain indefinitely.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

// Highest partition index this code will look up. GPT technically allows
// many more (128 is the common default), but nothing in this firmware can
// ever wire up more SCSI targets than S2S_MAX_TARGETS (8 or 16 depending
// on platform -- see scsi2sd.h), so there's no reason to search further.
#define PARTITION_TABLE_MAX_INDEX 16

typedef struct
{
    uint32_t startSector;
    uint32_t sectorCount;
    bool isGPT; // false = MBR
} partition_extent_t;

// Resolve partition `partitionNumber` (1-based) on the SD card to its
// sector extent. Tries GPT first (validating both the header and
// partition-entry-array CRC32 checksums; if the primary copy fails either
// check, logs a loud warning and retries the backup copy at the end of
// the disk before giving up), then falls back to MBR (indices 1-4 only --
// extended/logical MBR partitions are out of scope, MBR itself has no
// integrity check to offer).
//
// Returns false if the partition doesn't exist, or if a GPT-formatted
// card has both copies of its partition table corrupt (logged loudly
// either way -- this is not silent).
bool partitionTableResolve(uint32_t partitionNumber, partition_extent_t *out);

// ---- Validation helpers for PART:n, used by ZuluSCSI_disk.cpp before a
// resolved partition is allowed to back a SCSI target. Kept here (rather
// than in ZuluSCSI_disk.cpp) because they only need partition-extent and
// SD-card information, which this file already owns. ----

// Checks whether `startSector` lands on the SD card's preferred
// allocation-unit boundary (from the SD Status Register's AU_SIZE field,
// see ZuluSCSI.cpp's existing readSDS() use for speed class). Returns
// true (pass) if the card doesn't report a usable AU_SIZE at all --
// that's advisory hardware information, not something to hard-block a
// configuration over when unavailable. On failure, `auSizeSectorsOut` is
// filled with the AU size in sectors, for the caller's error message.
bool partitionTableCheckAlignment(uint32_t startSector, uint32_t *auSizeSectorsOut);

// Records `partitionNumber`/extent as target `targetIdx`'s current claim,
// for overlap checking against other targets. Overwrites any previous
// claim for the same targetIdx (e.g. a config reload), so a target never
// conflicts with its own prior registration.
void partitionTableRegisterClaim(int targetIdx, uint32_t partitionNumber, uint32_t startSector, uint32_t sectorCount);

// Clears a target's claim (e.g. it failed validation and won't be
// presented, or was reconfigured to a non-partition image).
void partitionTableClearClaim(int targetIdx);

// One conflicting claim, for building the overlap error message.
typedef struct
{
    int targetIdx;
    uint32_t partitionNumber;
} partition_conflict_t;

// Checks [startSector, startSector+sectorCount) against every OTHER
// target's currently registered claim (this target's own existing claim,
// if any, is ignored). Fills `conflicts` (capacity `maxConflicts`) and
// `conflictCount` with what it finds. Returns true if any conflict was
// found.
bool partitionTableCheckOverlap(int targetIdx, uint32_t startSector, uint32_t sectorCount,
                                 partition_conflict_t *conflicts, int maxConflicts, int *conflictCount);
