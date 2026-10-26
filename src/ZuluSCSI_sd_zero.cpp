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

static gate_state_t g_gate = GATE_UNKNOWN;
static uint32_t g_gate_unit_sectors = 0;

void sdEraseGateReset()
{
    g_gate = GATE_UNKNOWN;
    g_gate_unit_sectors = 0;
}

bool sdEraseReadsAsZero(uint32_t *unitSectors)
{
    if (g_gate == GATE_UNKNOWN)
    {
        g_gate = GATE_CLOSED;

        if (ini_getbool("SCSI", "SdTrim", false, CONFIGFILE))
        {
            // Whether erased blocks read back as zeros is a property of each
            // card (SD SCR register, which this SD driver cannot read), so it is
            // measured: erase a temporary file's blocks and look.
            sd_erase_measure_t result;
            if (!sdTrimMeasure(&result, false))
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
                logmsg("SdTrim: erased blocks read back as zeros, erase unit ", (int)(result.unit_sectors / 2),
                       " KB, slowest erase ", (int)result.worst_erase_ms, " ms -- zeroed ranges are released with the card's erase");
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

bool sdZeroSectors(ImageBackingStore &file, uint32_t bytesPerSector, uint32_t lba, uint32_t count, uint32_t *releasedSectors)
{
    if (releasedSectors)
    {
        *releasedSectors = 0;
    }

    uint32_t au = 0;
    uint32_t bgn = 0;
    uint8_t mode = 0;
    if (count == 0 || !sdEraseReadsAsZero(&au) || au == 0 || !file.linearSdMapping(&bgn, &mode))
    {
        return write_zero_sectors(file, bytesPerSector, lba, count);
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
            return write_zero_sectors(file, bytesPerSector, lba, count);
    }

    // Physical byte range (within the image) of the units that the request
    // covers completely. A unit that is only partly covered holds other
    // sectors' data and must not be touched.
    uint64_t first_unit = ((uint64_t)lba + unit_sectors - 1) / unit_sectors;
    uint64_t end_unit = ((uint64_t)lba + count) / unit_sectors;
    if (end_unit <= first_unit)
    {
        return write_zero_sectors(file, bytesPerSector, lba, count);
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
    if (erase_end <= erase_first)
    {
        return write_zero_sectors(file, bytesPerSector, lba, count);
    }

    // Back to image positions: the logical sectors in front of and behind the
    // erased range, including a unit the range starts or ends in, are
    // written as zeros the normal way.
    uint64_t erase_start_off = (erase_first - bgn) * 512;
    uint64_t erase_end_off = (erase_end - bgn) * 512;
    uint32_t head_end = (uint32_t)((erase_start_off + unit_bytes - 1) / unit_bytes * unit_sectors);
    uint32_t tail_start = (uint32_t)(erase_end_off / unit_bytes * unit_sectors);

    if (!write_zero_sectors(file, bytesPerSector, lba, head_end - lba))
    {
        return false;
    }

    file.flush();

    uint32_t chunk = SD_ZERO_ERASE_CHUNK_SECTORS / au * au;
    if (chunk == 0)
    {
        chunk = au;
    }

    for (uint64_t s = erase_first; s < erase_end; )
    {
        uint64_t e = s + chunk < erase_end ? s + chunk : erase_end;
        platform_reset_watchdog();

        if (!SD.card()->erase((uint32_t)s, (uint32_t)(e - 1)))
        {
            // The card refused: write zeros the normal way for everything
            // not erased yet, starting at the unit containing this chunk.
            uint32_t from = (uint32_t)(((s - bgn) * 512) / unit_bytes * unit_sectors);
            if (from < lba)
            {
                from = lba;
            }
            logmsg("SdTrim: erase failed at sector ", (int)s, ", writing the remaining zeros");
            return write_zero_sectors(file, bytesPerSector, from, lba + count - from);
        }

        if (releasedSectors)
        {
            *releasedSectors += (uint32_t)(e - s);
        }
        s = e;
    }

    return write_zero_sectors(file, bytesPerSector, tail_start, lba + count - tail_start);
}
