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

#include "ZuluSCSI_sd_trimcheck.h"
#include "ZuluSCSI_platform.h"
#include "ZuluSCSI.h"
#include "ZuluSCSI_disk.h"
#include <ZuluSCSI_log.h>
#include <string.h>

extern bool g_rawdrive_active;

// Non-destructive for the user's data: everything this check erases is in a
// temporary file that it creates itself and deletes afterwards.

// Log only when the caller asked for the full, verbose check.
#define VLOG(...) do { if (verbose) logmsg(__VA_ARGS__); } while (0)

#define TRIMCHECK_FILE "ZULUTRIM.TMP"
#define TRIMCHECK_PATTERN 0xA5

// The check uses at most this many sectors per erase unit; a card with a
// larger erase unit (allocation unit) cannot be tested.
#define TRIMCHECK_MAX_UNIT_SECTORS (32UL * 1024 * 1024 / 512)

// Number of consecutive erase units tested, to see the spread of erase times.
#define TRIMCHECK_UNITS 2

static bool is_power_of_two(uint32_t v)
{
    return v != 0 && (v & (v - 1)) == 0;
}

// Counts the bytes of one erase unit by value, reading it back raw from the card.
struct readback_t
{
    uint32_t zeros;
    uint32_t ones;
    uint32_t pattern;
    uint32_t other;
};

static bool read_unit(uint32_t first, uint32_t unit, readback_t &rb, uint32_t &read_ms, uint32_t &check_ms)
{
    memset(&rb, 0, sizeof(rb));
    const uint32_t chunk_sectors = sizeof(scsiDev.data) / 512;
    uint32_t read_us = 0;
    uint32_t check_us = 0;

    for (uint32_t done = 0; done < unit; done += chunk_sectors)
    {
        uint32_t count = unit - done < chunk_sectors ? unit - done : chunk_sectors;
        uint32_t t0 = micros();
        bool read_ok = SD.card()->readSectors(first + done, scsiDev.data, count);
        read_us += micros() - t0;
        if (!read_ok)
        {
            return false;
        }

        t0 = micros();
        uint32_t i = 0;
        if (((uintptr_t)scsiDev.data & 3) == 0)
        {
            // Whole words first: nearly every word is one of the three uniform values.
            const uint32_t *words = (const uint32_t *)scsiDev.data;
            for (; i + 4 <= count * 512; i += 4, words++)
            {
                uint32_t w = *words;
                if (w == 0x00000000) rb.zeros += 4;
                else if (w == 0xFFFFFFFF) rb.ones += 4;
                else if (w == 0x01010101 * TRIMCHECK_PATTERN) rb.pattern += 4;
                else break; // mixed word: classify the rest byte by byte
            }
        }
        for (; i < count * 512; i++)
        {
            uint8_t b = scsiDev.data[i];
            if (b == 0x00) rb.zeros++;
            else if (b == 0xFF) rb.ones++;
            else if (b == TRIMCHECK_PATTERN) rb.pattern++;
            else rb.other++;
        }
        check_us += micros() - t0;
        platform_reset_watchdog();
    }

    read_ms = read_us / 1000;
    check_ms = check_us / 1000;
    return true;
}

static void report_card()
{
    uint32_t sectors = SD.card()->sectorCount();
    logmsg("SD card: ", (int)(sectors / 2048), " MB");

    csd_t csd;
    if (SD.card()->readCSD(&csd))
    {
        if (csd.eraseSingleBlock())
            logmsg("  CSD erase granularity: single block");
        else
            logmsg("  CSD erase granularity: ", (int)csd.eraseSize(), " sectors");
    }

    sds_t sds = {0};
    if (SD.card()->readSDS(&sds))
    {
        logmsg("  SD status: AU size ", (int)sds.auSizeKB(), " KB, speed class ", (int)sds.speedClass(),
               ", discard ", sds.discard() ? "yes" : "no", ", FULE ", sds.fule() ? "yes" : "no",
               ", erase timeout ", (int)sds.eraseTimeout(), " s + ", (int)sds.eraseOffset(), " s per erase");
    }

    scr_t scr;
    memset(&scr, 0, sizeof(scr));
    if (SD.card()->readSCR(&scr))
    {
        logmsg("  SCR: SD spec version ", (int)scr.sdSpecVer(), ", data after erase reads as ",
               scr.dataAfterErase() ? "ONES (0xFF)" : "ZEROS (0x00)");
    }
    else
    {
        logmsg("  SCR: not available with this SD driver (see the measured read-back below)");
    }
}

bool sdTrimMeasure(sd_erase_measure_t *out, bool verbose)
{
    memset(out, 0, sizeof(*out));

    if (g_rawdrive_active)
    {
        // The temporary file lives in the card's file system, which the host
        // may be changing at the same time (two writers corrupt it).
        logmsg("SD card erase (TRIM) check not possible: a target gives the host direct access to the card's file system");
        return false;
    }

    scr_t scr;
    memset(&scr, 0, sizeof(scr));
    if (SD.card()->readSCR(&scr))
    {
        out->scr_known = true;
        out->scr_zero = !scr.dataAfterErase();
    }

    VLOG("");
    VLOG("SD card erase (TRIM) check -- the SCSI bus is not serviced while this runs");
    if (verbose)
    {
        report_card();
    }

    uint32_t unit = 8192; // sectors; replaced by the card's AU size when known
    sds_t sds = {0};
    if (SD.card()->readSDS(&sds) && sds.auSizeKB() > 0)
    {
        unit = (sds.auSizeKB() * 1024) / 512;
    }
    csd_t csd;
    if (SD.card()->readCSD(&csd) && !csd.eraseSingleBlock() && (uint32_t)csd.eraseSize() > unit)
    {
        unit = csd.eraseSize();
    }

    if (!is_power_of_two(unit) || unit > TRIMCHECK_MAX_UNIT_SECTORS)
    {
        logmsg("  Erase unit of ", (int)unit, " sectors cannot be tested, giving up");
        return false;
    }
    out->unit_sectors = unit;

    // Room for the test units plus one unit of slack to reach an aligned start,
    // and one more as neighbours to check for collateral erasure.
    const uint32_t file_sectors = (TRIMCHECK_UNITS + 2) * unit;
    uint64_t free_space = (uint64_t)SD.freeClusterCount() * SD.bytesPerCluster();
    if ((uint64_t)file_sectors * 512 + 1024 * 1024 > free_space)
    {
        logmsg("  Not enough free space for the temporary test file (", (int)((uint64_t)file_sectors * 512 / (1024 * 1024)), " MB), giving up");
        return false;
    }

    FsFile file = SD.open(TRIMCHECK_FILE, O_RDWR | O_CREAT | O_TRUNC);
    if (!file)
    {
        logmsg("  Cannot create ", TRIMCHECK_FILE, ", giving up");
        return false;
    }

    uint32_t begin = 0, end = 0;
    if (!file.preAllocate((uint64_t)file_sectors * 512) || !file.contiguousRange(&begin, &end))
    {
        logmsg("  No contiguous free space for the temporary test file, giving up");
        file.close();
        SD.remove(TRIMCHECK_FILE);
        return false;
    }

    // Fill it with a pattern, so that the effect of the erase is visible.
    memset(scsiDev.data, TRIMCHECK_PATTERN, sizeof(scsiDev.data));
    bool ok = true;
    for (uint64_t done = 0; ok && done < (uint64_t)file_sectors * 512; done += sizeof(scsiDev.data))
    {
        ok = file.write(scsiDev.data, sizeof(scsiDev.data)) == (int)sizeof(scsiDev.data);
        platform_reset_watchdog();
    }
    ok = ok && file.sync();

    uint32_t first = (begin + unit - 1) / unit * unit; // first unit-aligned sector inside the file
    if (!ok || first + (TRIMCHECK_UNITS + 1) * unit - 1 > end)
    {
        logmsg("  Cannot prepare the temporary test file, giving up");
        file.close();
        SD.remove(TRIMCHECK_FILE);
        return false;
    }

    VLOG("  Test: erasing ", TRIMCHECK_UNITS, " x ", (int)unit, " sectors (", (int)(unit / 2048), " MB each) at sector ", (int)first,
           ", previously filled with 0xA5");

    uint32_t verdict_zeros = 0, verdict_ones = 0, verdict_pattern = 0, verdict_other = 0;
    uint32_t units_done = 0;
    for (uint32_t u = 0; u < TRIMCHECK_UNITS; u++)
    {
        uint32_t unit_first = first + u * unit;
        uint32_t t0 = micros();
        bool erased = SD.card()->erase(unit_first, unit_first + unit - 1);
        uint32_t erase_us = micros() - t0;

        if (!erased)
        {
            logmsg("  Unit ", (int)u, ": the card refused the erase command, TRIM is not usable on this card");
            break;
        }

        readback_t rb;
        uint32_t read_ms = 0, check_ms = 0;
        if (!read_unit(unit_first, unit, rb, read_ms, check_ms))
        {
            logmsg("  Unit ", (int)u, ": reading back failed");
            break;
        }

        VLOG("  Unit ", (int)u, ": erase took ", (int)(erase_us / 1000), " ms; read back ", (int)(rb.zeros / 1024), " KB zeros, ",
               (int)(rb.ones / 1024), " KB 0xFF, ", (int)(rb.pattern / 1024), " KB unchanged, ", (int)(rb.other / 1024),
               " KB other; reading the unit from the card took ", (int)read_ms, " ms (plus ", (int)check_ms,
               " ms for comparing it in this check)");
        units_done++;
        if (erase_us / 1000 + 1 > out->worst_erase_ms)
        {
            out->worst_erase_ms = erase_us / 1000 + 1;
        }
        verdict_zeros += rb.zeros;
        verdict_ones += rb.ones;
        verdict_pattern += rb.pattern;
        verdict_other += rb.other;
    }

    // Neighbouring unit that must not have been touched.
    readback_t nb;
    uint32_t nb_ms = 0, nb_check_ms = 0;
    bool neighbour_ok = read_unit(first + TRIMCHECK_UNITS * unit, unit, nb, nb_ms, nb_check_ms) && nb.pattern == unit * 512;
    VLOG("  Neighbouring unit after the erased ones: ", neighbour_ok ? "untouched" : "CHANGED (the card erased more than asked)");

    if (verdict_zeros > 0 && verdict_ones == 0 && verdict_pattern == 0 && verdict_other == 0)
        VLOG("  Result: erased blocks read back as ZEROS -- erasing all-zero ranges does not change what the host reads");
    else if (verdict_ones > 0 && verdict_zeros == 0 && verdict_pattern == 0 && verdict_other == 0)
        VLOG("  Result: erased blocks read back as ONES (0xFF) -- erasing zero ranges WOULD change what the host reads");
    else if (verdict_pattern > 0 && verdict_zeros == 0 && verdict_ones == 0 && verdict_other == 0)
        VLOG("  Result: the data was unchanged after the erase -- the card defers or ignores erases");
    else if (verdict_zeros + verdict_ones + verdict_pattern + verdict_other > 0)
        VLOG("  Result: mixed read-back -- erasing is NOT safe for zero ranges on this card");

    if (out->scr_known && units_done == TRIMCHECK_UNITS)
    {
        bool measured_zero = verdict_zeros > 0 && verdict_ones == 0 && verdict_pattern == 0 && verdict_other == 0;
        bool measured_ones = verdict_ones > 0 && verdict_zeros == 0 && verdict_pattern == 0 && verdict_other == 0;
        if (measured_zero == out->scr_zero && (measured_zero || measured_ones))
            VLOG("  The card's SCR register agrees: erased data reads as ", out->scr_zero ? "zeros" : "ones");
        else
            VLOG("  The card's SCR register says erased data reads as ", out->scr_zero ? "zeros" : "ones", ", which DIFFERS from the measurement above -- trust the measurement");
    }

    file.close();
    SD.remove(TRIMCHECK_FILE);
    VLOG("SD card erase (TRIM) check finished");

    out->neighbour_ok = neighbour_ok;
    out->reads_zero = units_done == TRIMCHECK_UNITS && verdict_zeros > 0 &&
                      verdict_ones == 0 && verdict_pattern == 0 && verdict_other == 0;
    return units_done == TRIMCHECK_UNITS;
}

void sdTrimCheckRun()
{
    sd_erase_measure_t result;
    sdTrimMeasure(&result, true);
}
