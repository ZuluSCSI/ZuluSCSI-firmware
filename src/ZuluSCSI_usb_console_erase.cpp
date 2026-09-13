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

#include "ZuluSCSI_usb_console_erase.h"
#include "ZuluSCSI_platform.h"
#include "ZuluSCSI.h"
#include <ZuluSCSI_log.h>
#include <string.h>
#include <stdio.h>

// How long to wait while the serial interface is busy before giving up on
// printing more characters -- matches ZuluSCSI_usb_console_media.cpp.
#ifndef USB_CONSOLE_ERASE_SERIAL_PRINT_TIMEOUT_MS
#define USB_CONSOLE_ERASE_SERIAL_PRINT_TIMEOUT_MS 30
#endif

// Chunk size cap, independent of card size -- see the design discussion
// in project memory (project_as400_sdcard_trim_feature.md): a chunk size
// that scales with card size (e.g. a flat 1/100th) doesn't bound
// worst-case wait time on a very large card, which defeats the point of
// chunking (progress feedback + a bounded wait per step) in the first
// place. Chunk size actually used is min(total_sectors/100, this cap),
// so typical/small cards still get clean ~1% steps, while large cards
// get finer (more, smaller) chunks instead of a few huge ones.
//
// PROVISIONAL: real hardware testing (2026-09-14) showed a 512MB cap is
// too large -- a first chunk of ~300MB (1% of a ~30GB card, well under
// this cap) failed to finish erasing within the RP2350 SDIO driver's
// 30s per-call timeout. Shrunk to 16MB as a more conservative default
// pending real timing data on known-good media; revisit once there's a
// wider sample of real erase-throughput numbers to work from.
#define ERASE_CHUNK_CAP_SECTORS (16UL * 1024 * 1024 / 512)

#define TRIM_CONFIRM_WORD "TRIM"

static void serial_out(const char *str)
{
    if (!str || !*str) return;
    uint32_t remaining = (uint32_t)strlen(str);
    const uint8_t *p = (const uint8_t *)str;
    uint32_t timeout_start = millis();
    const uint32_t timeout_ms = USB_CONSOLE_ERASE_SERIAL_PRINT_TIMEOUT_MS;

    while ((uint32_t)(millis() - timeout_start) < timeout_ms && remaining > 0)
    {
        uint32_t sent = platform_write_to_serial((uint8_t *)p, remaining);
        if (sent > 0)
            timeout_start = millis();
        p += sent;
        remaining -= sent;
        platform_reset_watchdog();
    }
}

static void serial_println(const char *str)
{
    serial_out(str);
    serial_out("\r\n");
}

typedef enum
{
    ERASE_MENU_INACTIVE,
    ERASE_MENU_CONFIRM_TEXT, // waiting for the user to type TRIM + Enter
} erase_menu_state_t;

static erase_menu_state_t s_state = ERASE_MENU_INACTIVE;

// Up to strlen(TRIM_CONFIRM_WORD) characters, plus a little slack for a
// mistyped-but-still-short entry, plus NUL.
static char s_text_buf[8];
static int s_text_len = 0;

static void show_warning_and_prompt()
{
    platform_flush_usb_log();
    serial_println("");
    serial_println("  !! ERASE (TRIM) ENTIRE SD CARD !!");
    serial_println("  ================================================");
    serial_println("  This will PERMANENTLY DESTROY the entire content of the");
    serial_println("  SD card -- every image, every partition, the filesystem");
    serial_println("  itself, all of it. This cannot be undone.");
    serial_println("");
    serial_println("  The SCSI bus will not be serviced while this runs.");
    serial_println("");
    serial_out("  Type the word ");
    serial_out(TRIM_CONFIRM_WORD);
    serial_println(" (exactly, then Enter) to proceed, or anything else to cancel:");
}

// Erases the whole SD card in bounded-size chunks, printing progress and
// petting the watchdog between chunks (SdioCard::erase() itself also
// pets the watchdog while waiting for one chunk's busy state to clear --
// see lib/ZuluSCSI_platform_RP2MCU/sd_card_sdio.cpp). SD's erase command
// gives no native progress indication at all (unlike SCSI FORMAT UNIT),
// so chunking is the only way to report real progress here, not an
// afterthought bolted onto a single whole-card erase call.
static void run_erase()
{
    uint32_t total_sectors = SD.card()->sectorCount();
    if (total_sectors == 0)
    {
        serial_println("  Could not determine SD card size, aborting.");
        logmsg("---- SD card erase aborted: could not determine card size");
        return;
    }

    uint32_t chunk_sectors = total_sectors / 100;
    if (chunk_sectors == 0 || chunk_sectors > ERASE_CHUNK_CAP_SECTORS)
        chunk_sectors = ERASE_CHUNK_CAP_SECTORS;
    if (chunk_sectors > total_sectors)
        chunk_sectors = total_sectors;

    uint32_t total_chunks = (total_sectors + chunk_sectors - 1) / chunk_sectors;

    logmsg("---- Starting SD card erase: ", (int)total_sectors, " sectors, ",
           (int)total_chunks, " chunks of ", (int)chunk_sectors, " sectors each");
    // logmsg() only queues into the RAM log ring buffer -- it's drained to
    // the actual serial port lazily, normally as part of the main loop's
    // own polling. Since we're about to write directly to the serial port
    // below (and then block in a long busy-wait loop that never returns to
    // the main loop), force a synchronous flush now so the two output
    // paths can't interleave mid-line (this was seen for real: the queued
    // "chunks of N sectors each" message tore into the middle of the
    // "Erasing N sectors..." line printed just after it).
    platform_flush_usb_log();
    serial_println("");
    serial_out("  Erasing ");
    char numbuf[16];
    snprintf(numbuf, sizeof(numbuf), "%lu", (unsigned long)total_sectors);
    serial_out(numbuf);
    serial_println(" sectors...");

    uint32_t sector = 0;
    for (uint32_t chunk = 0; chunk < total_chunks; chunk++)
    {
        uint32_t first = sector;
        uint32_t count = (total_sectors - first < chunk_sectors) ? (total_sectors - first) : chunk_sectors;
        uint32_t last = first + count - 1;

        platform_reset_watchdog();

        if (!SD.card()->erase(first, last))
        {
            serial_println("");
            serial_println("  ERASE FAILED -- card may be left in an inconsistent state.");
            logmsg("---- SD card erase FAILED at sectors ", (int)first, "-", (int)last);
            return;
        }

        sector = last + 1;

        int percent = (int)(((uint64_t)(chunk + 1) * 100) / total_chunks);
        serial_out("\r  Erasing... ");
        char pctbuf[8];
        snprintf(pctbuf, sizeof(pctbuf), "%3d", percent);
        serial_out(pctbuf);
        serial_out("%");
    }

    serial_println("");
    serial_println("  Erase complete. Power-cycle or reboot before using the card.");
    logmsg("---- SD card erase complete: ", (int)total_sectors, " sectors erased");
}

bool serialEraseMenuActive()
{
    return s_state != ERASE_MENU_INACTIVE;
}

void serialEraseMenuEnter()
{
    s_text_len = 0;
    s_text_buf[0] = '\0';
    s_state = ERASE_MENU_CONFIRM_TEXT;
    show_warning_and_prompt();
}

void serialEraseMenuProcess(char c)
{
    if (s_state != ERASE_MENU_CONFIRM_TEXT)
    {
        s_state = ERASE_MENU_INACTIVE;
        return;
    }

    if (c == '\r' || c == '\n')
    {
        if (s_text_len == 0)
            return; // ignore bare CR/LF before any text was typed

        bool confirmed = (s_text_len == (int)strlen(TRIM_CONFIRM_WORD) &&
                           strncmp(s_text_buf, TRIM_CONFIRM_WORD, s_text_len) == 0);

        s_text_len = 0;
        s_text_buf[0] = '\0';
        s_state = ERASE_MENU_INACTIVE;

        if (confirmed)
        {
            run_erase();
        }
        else
        {
            serial_println("  Not confirmed -- SD card left untouched.");
            logmsg("---- SD card erase cancelled (confirmation text did not match)");
        }
        return;
    }

    if (s_text_len < (int)sizeof(s_text_buf) - 1)
    {
        s_text_buf[s_text_len++] = c;
        s_text_buf[s_text_len] = '\0';
    }
    else
    {
        // Typed text is already longer than TRIM_CONFIRM_WORD could ever
        // match -- no point accumulating further, but keep consuming
        // characters until Enter so they don't leak into the top-level
        // menu once this submenu exits.
    }
}
