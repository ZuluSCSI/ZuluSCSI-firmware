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

#include "ZuluSCSI_sd_scrub.h"
#include "ZuluSCSI_sd_zero.h"
#include "ZuluSCSI_sd_fsfree.h"
#include "ZuluSCSI_platform.h"
#include "ZuluSCSI.h"
#include "ZuluSCSI_disk.h"
#include "ZuluSCSI_config.h"
#include <ZuluSCSI_log.h>
#include <minIni.h>
#include <string.h>

// The scrubber runs in the main loop while the SCSI bus has been quiet for a
// while. It reads the card one erase unit (allocation unit, AU) at a time and
// releases the units that are entirely zeros with the card's ERASE command.
// Nothing changes for the host: erased blocks read back as zeros (checked by
// sdEraseReadsAsZero()). Because the main loop is single threaded and one unit
// is read and erased in a single call without handling any SCSI command in
// between, no write can slip in between the check and the erase. The call
// gives up as soon as a host selects the Zulu.

// The bus must have been quiet this long before a unit is looked at.
#define SCRUB_IDLE_MS 1000

// After working for some time the scrubber rests three times as long: it uses
// about a quarter of the card's time at most.
#define SCRUB_REST_FACTOR 3

// An erase that takes this many times the limit SD_ERASE_MAX_MS (measured
// at start) stops the scrubber.
#define SCRUB_ERASE_BAIL_FACTOR 4

// Double blink every two seconds while a pass is running, to tell it from
// SCSI activity.
#define SCRUB_LED_CYCLE_MS 2000

typedef enum
{
    SCRUB_OFF,
    SCRUB_RUNNING,
    SCRUB_RESTING // between two passes
} scrub_state_t;

static struct
{
    scrub_state_t state;
    uint32_t au_sectors;
    uint32_t au_count;
    uint32_t first_au;       // where this pass started, to cover the card over many short sessions
    uint32_t done_aus;
    uint32_t released_aus;
    uint32_t fs_free_aus;    // of them, units the file system reports as free
    bool fs_usable;          // [SCSI] SdScrubFreeSpace and sdFsFreeStart() succeeded
    uint32_t next_log_percent;
    uint32_t pause_hours;
    uint32_t next_step_ms;
    uint32_t resume_ms;
    bool led_on;
} s;

// After a card change the scrubber starts again for the new card, once the bus
// has been quiet for a while (measuring the new card takes a second).
#define SCRUB_RESTART_IDLE_MS 5000

static bool g_restart_pending = false;
static uint32_t g_last_activity_ms = 0;
static uint32_t g_last_sel_count = 0;

static bool time_reached(uint32_t now, uint32_t t)
{
    return (int32_t)(now - t) >= 0;
}

static bool bus_active()
{
    return scsiDev.phase != BUS_FREE || scsiDev.resetFlag || scsiDev.selFlag ||
           scsiStatusBSY() || scsiStatusSEL();
}

static bool all_zero(const uint8_t *data, uint32_t len)
{
    uint32_t i = 0;
    if (((uintptr_t)data & 3) == 0)
    {
        const uint32_t *words = (const uint32_t *)data;
        for (; i + 4 <= len; i += 4, words++)
        {
            if (*words != 0)
            {
                return false;
            }
        }
    }
    for (; i < len; i++)
    {
        if (data[i] != 0)
        {
            return false;
        }
    }
    return true;
}

static void led_set(bool on)
{
    if (on != s.led_on)
    {
        s.led_on = on;
        if (on) LED_ON(); else LED_OFF();
    }
}

static void start_pass()
{
    s.state = SCRUB_RUNNING;
    s.done_aus = 0;
    s.released_aus = 0;
    s.fs_free_aus = 0;
    s.next_log_percent = 10;
    s.first_au = (micros() ^ (millis() * 2654435761u)) % s.au_count;
    s.next_step_ms = millis();

    logmsg("SD scrubber: pass over ", (int)((uint64_t)s.au_count * s.au_sectors / 2048), " MB starts at ",
           (int)((uint64_t)s.first_au * s.au_sectors / 2048), " MB");
}

static void finish_pass(uint32_t now)
{
    s.state = SCRUB_RESTING;
    s.resume_ms = now + s.pause_hours * 3600UL * 1000UL;
    led_set(false);
    if (s.fs_usable)
    {
        logmsg("SD scrubber: 100% of the card scrubbed, ", (int)((uint64_t)s.released_aus * s.au_sectors / 2048),
               " MB released, of which ", (int)((uint64_t)s.fs_free_aus * s.au_sectors / 2048),
               " MB free space of the file system; the next pass starts in ", (int)s.pause_hours,
               " h (SdScrubPauseHours in zuluscsi.ini)");
    }
    else
    {
        logmsg("SD scrubber: 100% of the card scrubbed, ", (int)((uint64_t)s.released_aus * s.au_sectors / 2048),
               " MB released; the next pass starts in ", (int)s.pause_hours, " h (SdScrubPauseHours in zuluscsi.ini)");
    }
}

// Looks at one unit. Returns without doing anything to it if a host selects
// us in the meantime.
static void scrub_one_unit(uint32_t now)
{
    uint32_t au = (s.first_au + s.done_aus) % s.au_count;
    uint32_t first = au * s.au_sectors;
    const uint32_t chunk_sectors = sizeof(scsiDev.data) / 512;
    bool zero = true;

    // Space that the file system does not use needs no reading: whatever it
    // holds is not wanted.
    bool from_fs = s.fs_usable && sdFsRangeFree(first, s.au_sectors);

    uint32_t t0 = millis();
    for (uint32_t done = 0; !from_fs && done < s.au_sectors && zero; done += chunk_sectors)
    {
        if (bus_active())
        {
            g_last_activity_ms = millis();
            return; // try this unit again later
        }

        uint32_t count = s.au_sectors - done < chunk_sectors ? s.au_sectors - done : chunk_sectors;
        if (!SD.card()->readSectors(first + done, scsiDev.data, count))
        {
            logmsg("SD scrubber: reading sector ", first + done, " failed, stopping");
            s.state = SCRUB_OFF;
            led_set(false);
            return;
        }
        zero = all_zero(scsiDev.data, count * 512);
    }

    if (zero)
    {
        if (bus_active())
        {
            g_last_activity_ms = millis();
            return;
        }

        uint32_t e0 = millis();
        bool erased = SD.card()->erase(first, first + s.au_sectors - 1);
        uint32_t erase_ms = millis() - e0;

        if (!erased || erase_ms > SCRUB_ERASE_BAIL_FACTOR * SD_ERASE_MAX_MS)
        {
            logmsg("SD scrubber: erasing sector ", first, erased ? " took too long" : " failed", ", stopping");
            s.state = SCRUB_OFF;
            led_set(false);
            return;
        }
        s.released_aus++;
        if (from_fs)
        {
            s.fs_free_aus++;
        }
    }

    s.done_aus++;
    uint32_t end = millis();
    s.next_step_ms = end + (end - t0 + 1) * SCRUB_REST_FACTOR;

    uint32_t percent = (uint32_t)((uint64_t)s.done_aus * 100 / s.au_count);
    while (s.next_log_percent < 100 && percent >= s.next_log_percent)
    {
        logmsg("SD scrubber: ", (int)s.next_log_percent, "% of the card scrubbed, ",
               (int)((uint64_t)s.released_aus * s.au_sectors / 2048), " MB released so far");
        s.next_log_percent += 10;
    }

    if (s.done_aus >= s.au_count)
    {
        finish_pass(end);
    }
}

void sdTrimStart()
{
    memset(&s, 0, sizeof(s));

    bool trim_write_same = ini_getbool("SCSI", "SdTrim", false, CONFIGFILE);
    bool scrub = ini_getbool("SCSI", "SdScrub", false, CONFIGFILE);
    if (!trim_write_same && !scrub)
    {
        return;
    }

    // Measures what the card does when blocks are erased (a few seconds, the
    // host is not served meanwhile; this is why it is done here and not later).
    uint32_t au = 0;
    if (!sdEraseReadsAsZero(&au))
    {
        if (scrub)
        {
            logmsg("SD scrubber: not started, the card's erase cannot be used for it");
        }
        return;
    }

    if (!scrub)
    {
        return;
    }

    if (sdEraseWorstMs() > SD_ERASE_MAX_MS)
    {
        logmsg("SD scrubber: not started, erasing one unit of this card takes ", (int)sdEraseWorstMs(), " ms");
        return;
    }

    s.au_sectors = au;
    s.au_count = SD.card()->sectorCount() / au;
    if (s.au_count == 0)
    {
        return;
    }

    long hours = ini_getl("SCSI", "SdScrubPauseHours", 24, CONFIGFILE);
    s.pause_hours = hours < 1 ? 1 : (hours > 500 ? 500 : hours); // 500 h still fits the millisecond timer
    g_last_activity_ms = millis();
    g_last_sel_count = scsiDev.selCount;

    logmsg("SD scrubber: enabled, erase unit ", (int)(au / 2), " KB, waits for a quiet SCSI bus, pause between passes ",
           (int)s.pause_hours, " h");

    if (ini_getbool("SCSI", "SdScrubFreeSpace", false, CONFIGFILE))
    {
        s.fs_usable = sdFsFreeStart();
    }
    start_pass();
}

void sdScrubReset()
{
    led_set(false);
    sdFsFreeReset();
    s.fs_usable = false;
    s.state = SCRUB_OFF;
    g_restart_pending = true;
}

void sdScrubPoll()
{
    if (s.state == SCRUB_OFF && !g_restart_pending)
    {
        return;
    }

    uint32_t now = millis();

    if (!g_sdcard_present || bus_active() || scsiDev.selCount != g_last_sel_count)
    {
        g_last_sel_count = scsiDev.selCount;
        g_last_activity_ms = now;
        led_set(false);
        return;
    }

    if (g_restart_pending)
    {
        if (time_reached(now, g_last_activity_ms + SCRUB_RESTART_IDLE_MS))
        {
            g_restart_pending = false;
            sdTrimStart();
        }
        return;
    }

    if (s.state == SCRUB_RESTING)
    {
        if (time_reached(now, s.resume_ms))
        {
            start_pass();
        }
        return;
    }

    if (!time_reached(now, g_last_activity_ms + SCRUB_IDLE_MS))
    {
        led_set(false);
        return;
    }

    // Blink pattern: two short flashes every cycle.
    uint32_t phase = now % SCRUB_LED_CYCLE_MS;
    led_set(phase < 40 || (phase >= 120 && phase < 160));

    if (time_reached(now, s.next_step_ms))
    {
        scrub_one_unit(now);
    }
}
