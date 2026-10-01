/**
 * ZuluSCSI™ - Copyright (c) 2026 Rabbit Hole Computing™
 *
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

// Pure translation math for the AlignUnalignedAccesses "gapped" on-SD-card
// layout -- no SD card / filesystem access here at all, deliberately, so it
// can be (and is) unit-tested standalone on the host before ever touching
// real I/O. See ZuluSCSI_settings.h for the zuluscsi_align_unaligned_t enum
// this operates on, ImageBackingStore.cpp for where it's actually wired to
// SD access, and utils/as400_gapconv.c for the standalone conversion tool --
// the constants and layout here MUST stay in exact lockstep with that tool
// (CISC_SLOT_SIZE, PPC_GROUP_SECTORS, PPC_GROUP_PHYS_SIZE below correspond
// 1:1 to as400_gapconv.c's own #defines of the same names).
//
// Physical layout, matching as400_gapconv.c exactly:
//   CISC: every logical (520-byte) sector gets its own isolated 1024-byte
//         physical slot -- blockSize real bytes, then zero-filled padding.
//   PPC:  logical (522-byte) sectors are grouped 8-at-a-time into one
//         4608-byte (9 SD-sector) physical group -- the group's up to 8
//         sectors are packed back-to-back at the front (no per-sector
//         padding within a group), then the group's remaining bytes (432,
//         for a full 8-sector group) are zero-filled padding. A short
//         trailing group (fewer than 8 real sectors) still occupies the
//         full 4608-byte physical span, same as as400_gapconv.c.

#pragma once
#include <stdint.h>

// Pads/groups AS/400's 520 (CISC) or 522 (PPC) byte logical sectors out to
// a physical on-SD-card layout whose slots/groups land on the SD card's
// native 512-byte sector boundaries -- see README-as400.md for the
// user-facing setting this backs (per-device AlignUnalignedAccesses= ini
// key, ZuluSCSI_settings.h/.cpp). ALIGN_UNALIGNED_OFF must stay 0 so a
// zeroed/absent setting keeps today's tightly-packed layout.
// ALIGN_UNALIGNED_AUTO is resolved to CISC/PPC/OFF at ImageBackingStore
// construction time based on the device's actual block size (520/522/
// anything else) -- see gapLayoutResolveAuto() below. It exists purely
// so a profile-based device doesn't need its scheme spelled out (and
// kept in sync) by hand; it is deliberately NOT an implicit default just
// because AS400_DiskProfile is set, since that would silently start
// reinterpreting any pre-existing, un-gapped image's bytes on upgrade --
// it only ever takes effect when written explicitly.
// must be in the same order as align_unaligned_strings[] in ZuluSCSI_settings.cpp
typedef enum
{
    ALIGN_UNALIGNED_OFF = 0,
    ALIGN_UNALIGNED_CISC,
    ALIGN_UNALIGNED_PPC,
    ALIGN_UNALIGNED_AUTO,
} zuluscsi_align_unaligned_t;

// Resolves ALIGN_UNALIGNED_AUTO to the scheme matching blockSize (520->CISC,
// 522->PPC, anything else->OFF, since no other block size has a gapped
// layout defined at all). Passing anything other than ALIGN_UNALIGNED_AUTO
// returns it unchanged.
zuluscsi_align_unaligned_t gapLayoutResolveAuto(zuluscsi_align_unaligned_t requestedMode, uint32_t blockSize);

#define GAP_CISC_SLOT_SIZE 1024
#define GAP_PPC_GROUP_SECTORS 8
#define GAP_PPC_GROUP_PHYS_SIZE (9 * 512)

// Describes one maximal contiguous run of logical sectors, starting at the
// logical sector passed to gapLayoutNextRun(), that live inside a single
// physical unit (one CISC slot, or one PPC group) -- i.e. one run is
// exactly what a single SD-sector-granular physical transaction can cover
// in one contiguous shot, without straddling a padding gap.
typedef struct
{
    // How many consecutive logical sectors, starting at the sector passed
    // in, this run covers. Bounded by both the unit boundary and whatever
    // sector count the caller asked for -- never crosses into the next
    // physical unit.
    uint32_t sectorCount;

    // Byte offset of the start of this run's physical unit (slot/group),
    // relative to the start of the gapped image/partition.
    uint64_t unitPhysicalOffset;

    // Total physical size of this run's unit, in bytes (always
    // GAP_CISC_SLOT_SIZE or GAP_PPC_GROUP_PHYS_SIZE) -- always a multiple
    // of 512, i.e. a whole number of SD sectors.
    uint32_t unitPhysicalSize;

    // Byte offset within the unit where this run's real (non-padding)
    // data starts -- 0 for CISC (each unit holds exactly one sector), or
    // (sector's index within its group) * blockSize for PPC.
    uint32_t byteOffsetInUnit;

    // How many logical sectors this run's *unit* can hold at most (1 for
    // CISC, GAP_PPC_GROUP_SECTORS for PPC) -- lets a caller tell whether a
    // write covers a whole unit from its start (in which case the unit's
    // padding can simply be blasted fresh, no read-modify-write needed)
    // versus only part of one (in which case any other real sectors
    // sharing the same unit must be preserved via read-modify-write).
    uint32_t unitSectorCapacity;

    // This run's starting sector's index within its unit (0-based). A
    // write covers its whole unit from the start exactly when this is 0
    // and sectorCount == unitSectorCapacity.
    uint32_t sectorIndexInUnit;
} gap_layout_run_t;

// Computes the next run starting at logicalSector (0-based AS/400 logical
// sector index), covering at most sectorsRemaining sectors. Always returns
// at least 1 sector's worth (a single sector's own unit always exists).
// alignMode must not be ALIGN_UNALIGNED_OFF -- callers are expected to
// bypass this translation entirely in that case, matching today's
// tightly-packed layout with no translation needed.
void gapLayoutNextRun(zuluscsi_align_unaligned_t alignMode, uint32_t blockSize,
                      uint32_t logicalSector, uint32_t sectorsRemaining,
                      gap_layout_run_t *out);

// Total physical (gapped) byte size needed to store sectorCount logical
// sectors under the given alignment mode -- the size to actually allocate/
// preAllocate() on the SD card, and the real "does this partition have
// enough room" threshold (larger than sectorCount*blockSize by design).
// Returns sectorCount*blockSize unchanged for ALIGN_UNALIGNED_OFF.
uint64_t gapLayoutPhysicalSize(zuluscsi_align_unaligned_t alignMode, uint32_t blockSize, uint32_t sectorCount);

// Inverse of gapLayoutPhysicalSize(): how many whole logical sectors fit
// within physicalBytes of gapped physical space. Only counts whole,
// complete physical units (a partial trailing CISC slot or PPC group is
// dropped, not counted as a partial sector) -- this is what determines
// the SCSI-reported capacity of a raw partition/file with no declared
// disk profile, where "whatever physical space is available" becomes the
// device's capacity (see ZuluSCSI_disk.cpp's PART:n handling). Returns
// physicalBytes/blockSize unchanged for ALIGN_UNALIGNED_OFF.
uint32_t gapLayoutLogicalSectorsInPhysicalSize(zuluscsi_align_unaligned_t alignMode, uint32_t blockSize, uint64_t physicalBytes);
