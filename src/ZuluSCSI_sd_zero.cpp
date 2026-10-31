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

#include "ZuluSCSI_sd_zero.h"
#include "ZuluSCSI_sd_trimcheck.h"
#include "ZuluSCSI_gap_layout.h"
#include "ZuluSCSI_platform.h"
#include "ZuluSCSI.h"
#include "ZuluSCSI_disk.h"
#include "ZuluSCSI_config.h"
#include <ZuluSCSI_log.h>
#include <minIni.h>
#include <string.h>

// Largest range handed to one ERASE command, in sectors (16 MB). Erasing a
// whole large range in one go can take longer than the watchdog allows, and
// a card cannot be interrupted while it erases.
#define SD_ZERO_ERASE_CHUNK_SECTORS (16UL * 1024 * 1024 / 512)

typedef enum
{
    GATE_UNKNOWN,
    GATE_OPEN,
    GATE_CLOSED
} gate_state_t;

extern bool g_rawdrive_active;

static gate_state_t g_gate = GATE_UNKNOWN;
static uint32_t g_gate_unit_sectors = 0;
static uint32_t g_gate_worst_erase_ms = 0;
static bool g_writesame_trim = false; // [SCSI] SdTrim: use the gate for WRITE SAME
static bool g_scrub_setting = false;  // [SCSI] SdScrub: UNMAP is announced and honoured

void sdEraseGateReset()
{
    g_gate = GATE_UNKNOWN;
    g_gate_unit_sectors = 0;
    g_gate_worst_erase_ms = 0;
    g_writesame_trim = false;
    g_scrub_setting = false;
}

uint32_t sdEraseWorstMs()
{
    return g_gate_worst_erase_ms;
}

bool sdEraseReadsAsZero(uint32_t *unitSectors)
{
    if (g_gate == GATE_UNKNOWN)
    {
        g_gate = GATE_CLOSED;

        g_writesame_trim = ini_getbool("SCSI", "SdTrim", false, CONFIGFILE);
        g_scrub_setting = ini_getbool("SCSI", "SdScrub", false, CONFIGFILE);
        if (g_writesame_trim || g_scrub_setting)
        {
            // Whether erased blocks read back as zeros is a property of each
            // card (SD SCR register, which this SD driver cannot read), so it is
            // measured: erase a temporary file's blocks and look.
            sd_erase_measure_t result;
            scr_t scr;
            memset(&scr, 0, sizeof(scr));
            if (SD.card()->readSCR(&scr) && scr.dataAfterErase())
            {
                // The card says so itself, no need to test.
                logmsg("SdTrim: the card's SCR register says erased blocks read back as ones, zeros are written normally");
            }
            else if (g_rawdrive_active)
            {
                // The test writes a file into the card's file system, which
                // the host may be changing at the same time.
                logmsg("SdTrim: the card's erase is not tested, a target gives the host direct access to the card's file system. Zeros are written normally");
            }
            else if (!sdTrimMeasure(&result, false))
            {
                logmsg("SdTrim: the card's erase could not be tested, zeros are written normally");
            }
            else if (!result.reads_zero || !result.neighbour_ok)
            {
                logmsg("SdTrim: this card does not read erased blocks back as zeros (or erases more than asked), zeros are written normally");
            }
            else
            {
                g_gate = GATE_OPEN;
                g_gate_unit_sectors = result.unit_sectors;
                g_gate_worst_erase_ms = result.worst_erase_ms;
                logmsg("SdTrim: erased blocks read back as zeros, erase unit ", (int)(result.unit_sectors / 2),
                       " KB, slowest erase ", (int)result.worst_erase_ms, " ms");
            }
        }
    }

    if (g_gate == GATE_OPEN && unitSectors)
    {
        *unitSectors = g_gate_unit_sectors;
    }
    return g_gate == GATE_OPEN;
}

// Writes count logical sectors of zeros starting at lba.
static bool write_zero_sectors(ImageBackingStore &file, uint32_t bytesPerSector, uint32_t lba, uint32_t count)
{
    if (count == 0)
    {
        return true;
    }

    memset(scsiDev.data, 0, sizeof(scsiDev.data));
    const uint32_t sectors_per_buffer = sizeof(scsiDev.data) / bytesPerSector;
    if (sectors_per_buffer == 0)
    {
        return false; // sectors larger than the buffer: nothing could be written
    }

    if (!file.seek((uint64_t)lba * bytesPerSector))
    {
        return false;
    }

    while (count > 0)
    {
        uint32_t n = count < sectors_per_buffer ? count : sectors_per_buffer;
        if (file.write(scsiDev.data, n * bytesPerSector) != (ssize_t)(n * bytesPerSector))
        {
            return false;
        }
        count -= n;
        platform_reset_watchdog();
    }
    return true;
}

// Where a request to clear count logical sectors from lba can be released with
// the card's erase: whole erase units in absolute card sectors, which are
// entirely inside whole physical units of the image.
typedef struct
{
    uint64_t erase_first;   // first and one past the last SD sector to erase
    uint64_t erase_end;
    uint32_t head_end;      // logical sectors [lba, head_end) and
    uint32_t tail_start;    // [tail_start, lba + count) are not covered by it
    uint32_t bgn;           // first SD sector of the image
    uint32_t unit_bytes;    // physical unit of the image (see below)
    uint32_t unit_sectors;  // logical sectors per physical unit
} erase_plan_t;

// Returns false if no erase unit can be released (the image is not read
// directly from the card, the layout is not understood, or the range is too
// small).
static bool plan_erase(ImageBackingStore &file, uint32_t bytesPerSector, uint32_t lba, uint32_t count, uint32_t au, erase_plan_t *plan)
{
    uint32_t bgn = 0;
    uint8_t mode = 0;
    if (au == 0 || !file.linearSdMapping(&bgn, &mode))
    {
        return false;
    }

    // One physical unit is what a gapped layout stores together (its padding
    // is zero as well); without gaps it is one logical sector.
    uint32_t unit_bytes, unit_sectors;
    switch (mode)
    {
        case ALIGN_UNALIGNED_OFF:
            unit_bytes = bytesPerSector;
            unit_sectors = 1;
            break;
        case ALIGN_UNALIGNED_CISC:
            unit_bytes = GAP_CISC_SLOT_SIZE;
            unit_sectors = 1;
            break;
        case ALIGN_UNALIGNED_PPC:
            unit_bytes = GAP_PPC_GROUP_PHYS_SIZE;
            unit_sectors = GAP_PPC_GROUP_SECTORS;
            break;
        default:
            return false;
    }

    // Physical byte range (within the image) of the units that the request
    // covers completely. A unit that is only partly covered holds other
    // sectors' data and must not be touched.
    uint64_t first_unit = ((uint64_t)lba + unit_sectors - 1) / unit_sectors;
    uint64_t end_unit = ((uint64_t)lba + count) / unit_sectors;
    if (end_unit <= first_unit)
    {
        return false;
    }
    uint64_t phys_start = first_unit * unit_bytes;
    uint64_t phys_end = end_unit * unit_bytes;

    // Whole SD sectors inside that, then whole erase units in absolute card
    // sectors: the card may round an erase to its own units, so anything
    // else could erase neighbouring data.
    uint64_t sd_first = bgn + (phys_start + 511) / 512;
    uint64_t sd_end = bgn + phys_end / 512; // exclusive
    uint64_t erase_first = (sd_first + au - 1) / au * au;
    uint64_t erase_end = sd_end / au * au; // exclusive
    if (erase_end <= erase_first || erase_end > 0xFFFFFFFFULL) // the erase command takes 32 bit sector numbers
    {
        return false;
    }

    // Back to image positions: the logical sectors in front of and behind the
    // erased range, including a unit the range starts or ends in.
    uint64_t erase_start_off = (erase_first - bgn) * 512;
    uint64_t erase_end_off = (erase_end - bgn) * 512;

    plan->erase_first = erase_first;
    plan->erase_end = erase_end;
    plan->head_end = (uint32_t)((erase_start_off + unit_bytes - 1) / unit_bytes * unit_sectors);
    plan->tail_start = (uint32_t)(erase_end_off / unit_bytes * unit_sectors);
    plan->bgn = bgn;
    plan->unit_bytes = unit_bytes;
    plan->unit_sectors = unit_sectors;
    return true;
}

// Erases SD sectors [first, end) in chunks. Returns false if the card refused,
// with *failedAt the first sector of the chunk that was not erased.
static bool erase_sectors(uint64_t first, uint64_t end, uint32_t au, uint32_t *releasedSectors, uint64_t *failedAt)
{
    uint32_t chunk = SD_ZERO_ERASE_CHUNK_SECTORS / au * au;
    if (chunk == 0)
    {
        chunk = au;
    }

    for (uint64_t s = first; s < end; )
    {
        uint64_t e = s + chunk < end ? s + chunk : end;
        platform_reset_watchdog();

        if (!SD.card()->erase((uint32_t)s, (uint32_t)(e - 1)))
        {
            *failedAt = s;
            return false;
        }

        if (releasedSectors)
        {
            *releasedSectors += (uint32_t)(e - s);
        }
        s = e;
    }
    return true;
}

bool sdZeroSectors(ImageBackingStore &file, uint32_t bytesPerSector, uint32_t lba, uint32_t count, uint32_t *releasedSectors)
{
    if (releasedSectors)
    {
        *releasedSectors = 0;
    }

    uint32_t au = 0;
    erase_plan_t plan;
    const uint64_t request_end = (uint64_t)lba + count; // exclusive; may not fit 32 bits on a huge disk
    if (count == 0 || !sdEraseReadsAsZero(&au) || !g_writesame_trim ||
        !plan_erase(file, bytesPerSector, lba, count, au, &plan))
    {
        return write_zero_sectors(file, bytesPerSector, lba, count);
    }

    // The sectors in front of and behind the erased range, including a unit
    // the range starts or ends in, are written as zeros the normal way.
    if (!write_zero_sectors(file, bytesPerSector, lba, plan.head_end - lba))
    {
        return false;
    }

    file.flush();

    uint64_t failed_at = 0;
    if (!erase_sectors(plan.erase_first, plan.erase_end, au, releasedSectors, &failed_at))
    {
        // The card refused: write zeros the normal way for everything
        // not erased yet, starting at the unit containing this chunk.
        uint32_t from = (uint32_t)(((failed_at - plan.bgn) * 512) / plan.unit_bytes * plan.unit_sectors);
        if (from < lba)
        {
            from = lba;
        }
        logmsg("SdTrim: erase failed at sector ", failed_at, ", writing the remaining zeros");
        return write_zero_sectors(file, bytesPerSector, from, (uint32_t)(request_end - from));
    }

    return write_zero_sectors(file, bytesPerSector, plan.tail_start, (uint32_t)(request_end - plan.tail_start));
}

bool sdUnmapAvailable(uint32_t *unitSectors)
{
    // Only looks at the result of the measurement that sdTrimStart() makes
    // after boot and after a card change: measuring needs a temporary file on
    // the card, which must not happen while a host is using the card.
    if (g_gate != GATE_OPEN || !g_scrub_setting || g_gate_unit_sectors == 0 ||
        g_gate_worst_erase_ms > SD_ERASE_MAX_MS)
    {
        return false;
    }
    if (unitSectors)
    {
        *unitSectors = g_gate_unit_sectors;
    }
    return true;
}

void sdUnmapSectors(ImageBackingStore &file, uint32_t bytesPerSector, uint32_t lba, uint32_t count, uint32_t *releasedSectors)
{
    if (releasedSectors)
    {
        *releasedSectors = 0;
    }

    uint32_t au = 0;
    erase_plan_t plan;
    if (count == 0 || !sdUnmapAvailable(&au) || !plan_erase(file, bytesPerSector, lba, count, au, &plan))
    {
        return;
    }

    // Anything the image still has cached for this range must reach the card
    // before it is erased, or it would be written back over the erased range.
    file.flush();

    uint64_t failed_at = 0;
    if (!erase_sectors(plan.erase_first, plan.erase_end, au, releasedSectors, &failed_at))
    {
        // An UNMAP is only a hint, the data of a range that could not be
        // released simply stays as it is.
        logmsg("SdTrim: erase for UNMAP failed at sector ", failed_at);
    }
}
