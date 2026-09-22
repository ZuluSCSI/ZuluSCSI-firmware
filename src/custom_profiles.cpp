/**
 * ZuluSCSI™ - Copyright (c) 2025 Rabbit Hole Computing™
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

// Splitting a multi-section .ini definitions file into one standalone
// profile file per [Section].

#include "custom_profiles.h"

#if defined(PLATFORM_AS400) && defined(ZULUSCSI_MCU_RP23XX)

#include "ZuluSCSI_config.h"
#include "ZuluSCSI_log.h"
#include "ZuluSCSI_platform.h"

#include <SdFat.h>
#include <minIni.h>
#include <stdio.h>
#include <string.h>

// Use the SdFs instance from the main program
extern SdFs SD;

// State carried through ini_browse()'s per-key callback below.
struct custom_profile_split_state_t
{
    FsFile out;                   // the <Section>.ini currently being written
    char section[MAX_FILE_PATH];  // section name that 'out' belongs to
    int sections;                 // sections written so far
    int keys;                     // key/value pairs written so far
    bool section_failed;          // the current section could not be written
    bool error;                   // sticky: at least one section failed
};

// Build "<CUSTOM_PROFILES_DIR>/<section>.ini", replacing anything that is not
// legal in a FAT name with '_'. Section names in the definitions file are
// plain part numbers ("59H7001", "27H1711-170W-2"), but a hand-edited file
// must not be able to write outside CUSTOM_PROFILES_DIR.
static void custom_profile_path(const char *section, char *path, size_t pathsize)
{
    size_t pos = (size_t)snprintf(path, pathsize, "%s/", CUSTOM_PROFILES_DIR);
    const char *illegal = "\\/:*?\"<>|";

    while (*section != '\0' && pos + 5 < pathsize) // 5 = ".ini" + terminator
    {
        char c = *section++;
        path[pos++] = (c < ' ' || strchr(illegal, c) != NULL) ? '_' : c;
    }

    snprintf(path + pos, pathsize - pos, ".ini");
}

// Write one line to the section file, logging and flagging any short write.
static bool custom_profile_write(custom_profile_split_state_t *state, const char *line, size_t len)
{
    if (state->out.write(line, len) == len)
    {
        return true;
    }

    logmsg("---- ERROR: write to ", CUSTOM_PROFILES_DIR, "/", state->section,
           ".ini failed, sdErrorCode: ", (int)SD.sdErrorCode());
    state->error = true;
    return false;
}

// ini_browse() callback: called once per key/value pair, together with the
// section that pair belongs to. minIni walks the file top to bottom, so a
// change of section name means the previous section is complete and its file
// can be closed. Comments and blank lines never reach this callback, so the
// generated files carry only the [Section] header and its key/value pairs.
static int custom_profile_split_callback(const char *section, const char *key, const char *value, void *userdata)
{
    custom_profile_split_state_t *state = (custom_profile_split_state_t *)userdata;

    // Keys appearing before the first [Section] header have no file to go into
    if (section == NULL || section[0] == '\0')
    {
        return 1;
    }

    // state->section starts out empty and section names never are, so the very
    // first pair opens a file too. Deliberately keyed on the name alone rather
    // than on out.isOpen(): a section whose file failed must stay skipped for
    // its remaining keys instead of being reopened (and re-truncated) per key.
    if (strcmp(state->section, section) != 0)
    {
        if (state->out.isOpen())
        {
            state->out.close();
        }

        snprintf(state->section, sizeof(state->section), "%s", section);
        state->section_failed = false;

        char path[MAX_FILE_PATH];
        custom_profile_path(section, path, sizeof(path));

        state->out = SD.open(path, O_WRONLY | O_CREAT | O_TRUNC);
        if (!state->out.isOpen())
        {
            logmsg("---- ERROR: could not create custom profile file '", path,
                   "', sdErrorCode: ", (int)SD.sdErrorCode());
            state->section_failed = true;
            state->error = true;
            return 1; // keep browsing, the remaining profiles may still be writable
        }

        char header[MAX_FILE_PATH + 4];
        int headerlen = snprintf(header, sizeof(header), "[%s]\r\n", section);
        if (!custom_profile_write(state, header, (size_t)headerlen))
        {
            state->section_failed = true;
            state->out.close();
            return 1;
        }

        state->sections++;

        // Creating a directory entry per profile, on top of the sequential scan
        // of the definitions file, is slow enough on some cards to matter while
        // the boot watchdog is armed.
        platform_reset_watchdog();
    }

    if (state->section_failed || !state->out.isOpen())
    {
        return 1; // this section's file is unusable, skip the rest of its keys
    }

    // minIni hands section, key and value back inside one INI_BUFFERSIZE
    // buffer, so a very long line is already truncated by the time it gets
    // here -- the same limit the firmware reads these profiles under, which is
    // why the captures are split into SPD_0/SPD_1/..._chunks keys to begin with.
    char line[INI_BUFFERSIZE + 8];
    int linelen = snprintf(line, sizeof(line), "%s = %s\r\n", key, value);
    if (linelen > 0 && custom_profile_write(state, line, (size_t)linelen))
    {
        state->keys++;
    }
    else
    {
        // A half-written profile is worse than none: give up on this section
        // rather than emitting a file with a hole in the middle of it.
        state->section_failed = true;
        state->out.close();
    }

    return 1;
}

void splitCustomProfileDefinitions()
{
    FsFile src = SD.open(AS400_PROFILES_FILE, O_RDONLY);
    bool src_usable = src.isOpen() && src.fileSize() > 0;
    if (src.isOpen())
    {
        src.close();
    }

    if (!src_usable)
    {
        // Not an error: a card that does not use custom profiles has no
        // definitions file to split. A missing file is reported loudly where it
        // actually matters, when a [SCSIn] asks for a profile by name
        // (see loadAS400ProfileFromFile() in custom_vendor_inquiry.cpp).
        return;
    }

    FsFile dir = SD.open(CUSTOM_PROFILES_DIR, O_RDONLY);
    bool dir_usable = dir.isOpen() && dir.isDir();
    if (dir.isOpen())
    {
        dir.close();
    }

    if (!dir_usable && !SD.mkdir(CUSTOM_PROFILES_DIR))
    {
        logmsg("ERROR: could not create directory ", CUSTOM_PROFILES_DIR,
               ", sdErrorCode: ", (int)SD.sdErrorCode(),
               " -- custom profiles not split out of ", AS400_PROFILES_FILE);
        return;
    }

    // static, not a stack local: this runs at boot from the same call chain
    // that has previously overflowed the stack on real hardware (see the note
    // in readProfileHexField(), custom_vendor_inquiry.cpp), and the state holds
    // an FsFile plus a MAX_FILE_PATH name. Safe as static: this function is
    // only ever called sequentially, never reentrantly.
    static custom_profile_split_state_t state;
    state.section[0] = '\0';
    state.section_failed = false;
    state.sections = 0;
    state.keys = 0;
    state.error = false;

    logmsg("Splitting ", AS400_PROFILES_FILE, " into ", CUSTOM_PROFILES_DIR, "/");
    platform_reset_watchdog();

    if (!ini_browse(custom_profile_split_callback, &state, AS400_PROFILES_FILE))
    {
        logmsg("-- ERROR: could not read ", AS400_PROFILES_FILE,
               " to split out custom profiles, sdErrorCode: ", (int)SD.sdErrorCode());
        state.error = true;
    }

    if (state.out.isOpen())
    {
        state.out.close();
    }

    logmsg("---- Wrote ", state.sections, " custom profile file(s), ", state.keys, " key/value pairs");

    if (state.error)
    {
        logmsg("-- WARNING: not all custom profiles in ", AS400_PROFILES_FILE,
               " could be written to ", CUSTOM_PROFILES_DIR, "/");
        return; // leave it in place so the next boot retries the split
    }

    if (state.sections == 0)
    {
        // Nothing was recognised in it. Renaming here would hide a malformed
        // file rather than mark a processed one, so leave it where it is.
        logmsg("-- WARNING: no profile sections found in ", AS400_PROFILES_FILE);
        return;
    }

    // Mark the definitions file as processed by renaming it, so the split does
    // not run again on every boot. Readers of the definitions file fall back to
    // this name (see as400ProfilesFile() in custom_vendor_inquiry.cpp), so the
    // profiles stay reachable afterwards.
    if (SD.exists(AS400_PROFILES_FILE_PROCESSED) && !SD.remove(AS400_PROFILES_FILE_PROCESSED))
    {
        logmsg("-- WARNING: could not replace ", AS400_PROFILES_FILE_PROCESSED,
               ", leaving ", AS400_PROFILES_FILE, " in place");
        return;
    }

    if (!SD.rename(AS400_PROFILES_FILE, AS400_PROFILES_FILE_PROCESSED))
    {
        logmsg("-- WARNING: could not rename ", AS400_PROFILES_FILE, " to ",
               AS400_PROFILES_FILE_PROCESSED, ", sdErrorCode: ", (int)SD.sdErrorCode(),
               " -- it will be split again on the next boot");
        return;
    }

    logmsg("---- ", AS400_PROFILES_FILE, " -> ", AS400_PROFILES_FILE_PROCESSED);
}

#endif // PLATFORM_AS400 && ZULUSCSI_MCU_RP23XX
