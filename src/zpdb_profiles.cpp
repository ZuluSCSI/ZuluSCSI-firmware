/**
 * Copyright (C) 2026 Rabbit Hole Computing LLC
 *
 * This file is part of ZuluSCSI
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

#include "zpdb_profiles.h"
#include "zpdb.h"
#include "zpdb_flash.h"

#include "ZuluSCSI_log.h"
#include <ZuluSCSI_platform.h>

#include <SdFat.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

extern SdFs SD;

// ---------------------------------------------------------------- state

static ZpdbStore g_store;

// Per-target binding: a 16-byte section reference and a flag, and that is the
// whole per-target cost of a profile -- this is what replaces the per-target
// copies of the VPD/SPD/MODE SENSE payloads. The profile name is deliberately
// not kept here; it is already in flash, and zpdbProfileName() reads it back
// on the rare occasions something wants to print it.
static struct
{
    zpdb_section_ref_t section;
    bool bound;
} g_bound[S2S_MAX_TARGETS];


// ---------------------------------------------------------------- helpers

static bool endsWithIgnoreCase(const char *name, const char *suffix)
{
    size_t n = strlen(name), s = strlen(suffix);
    return n >= s && strcasecmp(name + n - s, suffix) == 0;
}

// Parse space/comma separated hex bytes. Returns bytes produced, or -1 if a
// token is not a hex pair -- a silent 0 would turn a typo into an empty page.
static int parseHexLine(const char *str, uint8_t *buf, int maxlen)
{
    int count = 0;

    while (*str != '\0')
    {
        while (*str == ' ' || *str == '\t' || *str == ',') str++;
        if (*str == '\0') break;

        int value = 0, digits = 0;
        while (digits < 2)
        {
            char c = *str;
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;

            value = (value << 4) | d;
            digits++;
            str++;
        }

        if (digits == 0) return -1;             // not a hex token at all
        if (count >= maxlen) return -1;         // caller's buffer is too small
        buf[count++] = (uint8_t)value;
    }

    return count;
}

static bool looksLikeHexList(const char *value)
{
    int tokens = 0;
    while (*value != '\0')
    {
        while (*value == ' ' || *value == '\t' || *value == ',') value++;
        if (*value == '\0') break;

        int digits = 0;
        while (isxdigit((unsigned char)*value)) { value++; digits++; }
        if (digits != 2) return false;
        tokens++;
    }
    return tokens >= 2;
}

// The key schema, mirroring SCHEMA in utils/zpdb_build.py. Syntax alone
// cannot decide these: "Revision = 1644" is an ASCII revision string while
// "LogSense31PageLength = 0" is a number.
static zpdb_type_t keyType(const char *key, const char *value)
{
    if (value[0] == '\0')
        return ZPDB_TYPE_NULL;

    if (strcasecmp(key, "Vendor") == 0 || strcasecmp(key, "Product") == 0 ||
        strcasecmp(key, "Revision") == 0 || strcasecmp(key, "Feature") == 0 ||
        strcasecmp(key, "Serial") == 0 || strcasecmp(key, "PartNumber") == 0)
        return ZPDB_TYPE_STR;

    if (strcasecmp(key, "Sectors") == 0)
        return ZPDB_TYPE_U64;

    if (strcasecmp(key, "BlockSize") == 0 || endsWithIgnoreCase(key, "PageLength") ||
        endsWithIgnoreCase(key, "PageListLength"))
        return ZPDB_TYPE_LONG;

    if (strncasecmp(key, "SPD", 3) == 0 || strncasecmp(key, "VPD", 3) == 0 ||
        strncasecmp(key, "ModeSense", 9) == 0 || strncasecmp(key, "LogSense", 8) == 0)
        return ZPDB_TYPE_DATA;

    if (looksLikeHexList(value))
        return ZPDB_TYPE_DATA;

    // Fall back to a number when it reads as one, a string otherwise.
    const char *p = value;
    if (*p == '+' || *p == '-') p++;
    if (*p == '\0') return ZPDB_TYPE_STR;
    for (; *p != '\0'; p++)
    {
        if (*p < '0' || *p > '9') return ZPDB_TYPE_STR;
    }
    return ZPDB_TYPE_LONG;
}

// Splits "ModeSense3F_0" into base "ModeSense3F" and index 0. Returns false
// for a plain key. "Key_chunks" is reported through *is_count instead: the
// count is implied by the merged value's length, so it is not stored.
static bool splitChunkKey(const char *key, char *base, size_t base_size,
                          long *index, bool *is_count)
{
    *is_count = false;

    const char *underscore = strrchr(key, '_');
    if (underscore == nullptr || underscore == key)
        return false;

    if (strcasecmp(underscore + 1, "chunks") == 0)
    {
        *is_count = true;
        return true;
    }

    const char *digits = underscore + 1;
    if (*digits == '\0') return false;
    for (const char *p = digits; *p != '\0'; p++)
    {
        if (*p < '0' || *p > '9') return false;
    }

    size_t len = (size_t)(underscore - key);
    if (len >= base_size) return false;

    memcpy(base, key, len);
    base[len] = '\0';
    *index = strtol(digits, nullptr, 10);
    return true;
}

// ---------------------------------------------------------------- ingest

struct IngestScratch
{
    uint8_t *section;
    uint8_t *page;
    uint8_t *page0;
    char *line;
    uint8_t *binary;
    uint32_t binary_size;
};

// Reads one line, without its terminator. Returns false at end of file.
// `overflow` is set when the line did not fit -- the caller fails the file
// rather than quietly using a truncated value.
static bool readLine(FsFile &file, char *buf, size_t size, bool *overflow)
{
    size_t len = 0;
    int c;

    *overflow = false;

    while ((c = file.read()) >= 0)
    {
        if (c == '\n')
        {
            buf[len] = '\0';
            return true;
        }
        if (c == '\r')
            continue;

        if (len + 1 >= size)
        {
            *overflow = true;
            // Consume the rest of the line so the parse can report and move on.
            while ((c = file.read()) >= 0 && c != '\n') { }
            buf[len] = '\0';
            return true;
        }
        buf[len++] = (char)c;
    }

    buf[len] = '\0';
    return len > 0;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t')) end--;
    *end = '\0';
    return s;
}

// Parse one .ini file into the writer. Returns false when the file should be
// moved to /failed; sections already committed by endSection() stay in the
// store, which is noted in the log.
static bool ingestFile(FsFile &file, const char *filename, ZpdbWriter &writer,
                       IngestScratch &scratch, uint32_t *hashes, uint32_t *hash_count)
{
    bool in_section = false;
    bool file_ok = true;
    char chunk_base[ZPDB_MAX_NAME] = { 0 };
    long expect_chunk = -1;
    int lineno = 0;

    for (;;)
    {
        bool overflow = false;
        if (!readLine(file, scratch.line, ZPDB_INI_LINE_MAX, &overflow))
            break;

        lineno++;

        if (overflow)
        {
            dbgmsg("---- ZPDB: ", filename, ":", (int)lineno, " is longer than ",
                   (int)ZPDB_INI_LINE_MAX, " characters");
            file_ok = false;
            break;
        }

        char *line = trim(scratch.line);
        if (line[0] == '\0' || line[0] == ';' || line[0] == '#')
            continue;

        if (line[0] == '[')
        {
            char *close = strchr(line, ']');
            if (close == nullptr)
            {
                dbgmsg("---- ZPDB: ", filename, ":", (int)lineno, " has no closing ']'");
                file_ok = false;
                break;
            }
            *close = '\0';
            char *name = trim(line + 1);

            if (in_section && !writer.endSection())
            {
                file_ok = false;
                break;
            }
            in_section = false;
            expect_chunk = -1;

            uint32_t hash = zpdb_hash(name);
            for (uint32_t i = 0; i < *hash_count; i++)
            {
                if (hashes[i] == hash)
                {
                    dbgmsg("---- ZPDB: ", filename, " defines profile '", name,
                           "' which another file already defined");
                    file_ok = false;
                    break;
                }
            }
            if (!file_ok) break;

            if (*hash_count >= ZPDB_MAX_PROFILES)
            {
                dbgmsg("---- ZPDB: more than ", (int)ZPDB_MAX_PROFILES,
                       " profiles, '", name, "' was not stored");
                file_ok = false;
                break;
            }

            if (!writer.beginSection(name))
            {
                file_ok = false;
                break;
            }

            hashes[(*hash_count)++] = hash;
            in_section = true;
            continue;
        }

        char *equals = strchr(line, '=');
        if (equals == nullptr)
        {
            dbgmsg("---- ZPDB: ", filename, ":", (int)lineno, " has no '='");
            file_ok = false;
            break;
        }

        *equals = '\0';
        char *key = trim(line);
        char *value = trim(equals + 1);

        if (!in_section)
        {
            dbgmsg("---- ZPDB: ", filename, ":", (int)lineno, " has key '", key,
                   "' outside any [profile] section");
            file_ok = false;
            break;
        }

        char base[ZPDB_MAX_NAME];
        long index = 0;
        bool is_count = false;
        bool chunked = splitChunkKey(key, base, sizeof(base), &index, &is_count);

        if (chunked && is_count)
            continue;   // implied by the merged value's length; not stored

        bool ok;
        if (chunked)
        {
            int len = parseHexLine(value, scratch.binary, (int)scratch.binary_size);
            if (len < 0)
            {
                dbgmsg("---- ZPDB: ", filename, ":", (int)lineno, " key '", key,
                       "' is not a list of hex bytes");
                file_ok = false;
                break;
            }

            if (index == 0)
            {
                strncpy(chunk_base, base, sizeof(chunk_base) - 1);
                chunk_base[sizeof(chunk_base) - 1] = '\0';
                expect_chunk = 1;
                ok = writer.addData(base, scratch.binary, (uint32_t)len);
            }
            else if (index == expect_chunk && strcasecmp(base, chunk_base) == 0)
            {
                expect_chunk++;
                ok = writer.appendData(scratch.binary, (uint32_t)len);
            }
            else
            {
                dbgmsg("---- ZPDB: ", filename, ":", (int)lineno, " key '", key,
                       "' is out of chunk order, expected ", (int)expect_chunk);
                file_ok = false;
                break;
            }
        }
        else
        {
            expect_chunk = -1;

            switch (keyType(key, value))
            {
                case ZPDB_TYPE_NULL:
                    ok = writer.addNull(key);
                    break;

                case ZPDB_TYPE_STR:
                    ok = writer.addString(key, value);
                    break;

                case ZPDB_TYPE_LONG:
                    ok = writer.addLong(key, (int32_t)strtol(value, nullptr, 0));
                    break;

                case ZPDB_TYPE_U64:
                    ok = writer.addU64(key, (uint64_t)strtoull(value, nullptr, 0));
                    break;

                default:
                {
                    int len = parseHexLine(value, scratch.binary, (int)scratch.binary_size);
                    if (len < 0)
                    {
                        dbgmsg("---- ZPDB: ", filename, ":", (int)lineno, " key '", key,
                               "' is not a list of hex bytes");
                        file_ok = false;
                        ok = false;
                        break;
                    }
                    ok = writer.addData(key, scratch.binary, (uint32_t)len);
                    break;
                }
            }
        }

        if (!ok)
        {
            if (file_ok)
            {
                dbgmsg("---- ZPDB: ", filename, ":", (int)lineno, " key '", key,
                       "' could not be stored");
            }
            file_ok = false;
            break;
        }
    }

    if (in_section)
    {
        if (file_ok)
        {
            file_ok = writer.endSection();
        }
        else
        {
            // Drop the half-built section rather than committing a profile
            // that is missing whatever came after the bad line.
            writer.abortSection();
        }
    }

    return file_ok && !writer.failed();
}

// Find the next .ini file sitting directly in the profile directory.
// Processed files are moved out as they go, so re-scanning from the start
// each time is what keeps the iteration valid across those renames.
static bool nextProfileFile(char *name, size_t name_size)
{
    FsFile dir, file;

    if (!dir.open(ZPDB_PROFILE_DIR))
        return false;

    bool found = false;
    while (!found && file.openNext(&dir, O_RDONLY))
    {
        char candidate[ZPDB_MAX_NAME];
        if (!file.isDirectory() && file.getName(candidate, sizeof(candidate)) &&
            endsWithIgnoreCase(candidate, ".ini"))
        {
            if (strlen(candidate) < name_size)
            {
                strcpy(name, candidate);
                found = true;
            }
            else
            {
                dbgmsg("---- ZPDB: file name '", candidate, "' is too long, skipped");
            }
        }
        file.close();
    }

    dir.close();
    return found;
}

static uint32_t countProfileFiles()
{
    FsFile dir, file;
    uint32_t count = 0;

    if (!dir.open(ZPDB_PROFILE_DIR))
        return 0;

    while (file.openNext(&dir, O_RDONLY))
    {
        char name[ZPDB_MAX_NAME];
        if (!file.isDirectory() && file.getName(name, sizeof(name)) &&
            endsWithIgnoreCase(name, ".ini"))
        {
            count++;
        }
        file.close();
    }

    dir.close();
    return count;
}

static void moveProfileFile(const char *name, bool ok)
{
    char from[256], to[256];
    const char *dir = ok ? ZPDB_LOADED_DIR : ZPDB_FAILED_DIR;

    if (!SD.exists(dir) && !SD.mkdir(dir))
    {
        dbgmsg("---- ZPDB: could not create ", dir, ", leaving ", name, " in place");
        return;
    }

    snprintf(from, sizeof(from), "%s/%s", ZPDB_PROFILE_DIR, name);
    snprintf(to, sizeof(to), "%s/%s", dir, name);

    if (SD.exists(to) && !SD.remove(to))
    {
        dbgmsg("---- ZPDB: could not replace ", to, ", leaving ", name, " in place");
        return;
    }

    if (!SD.rename(from, to))
        dbgmsg("---- ZPDB: could not move ", from, " to ", to);
    else
        dbgmsg("---- ZPDB: ", name, " -> ", dir);
}

static void rebuildStore(uint32_t file_count, uint8_t *scratch_buf)
{
    logmsg("-- ZuluSCSI Profile Database erasing and rebuilding the profile store from ", (int)file_count,
           " file(s) in ", ZPDB_PROFILE_DIR);

    // Everything below works in the caller's buffer; the store allocates
    // nothing. Hashes are taken from the front so they inherit whatever
    // alignment the caller's buffer has (the contract asks for 4-byte), and
    // the byte scratch follows on a 4-byte boundary.
    uint32_t *hashes = (uint32_t *)scratch_buf;
    uint8_t *scratch_mem = scratch_buf + ZPDB_MAX_PROFILES * sizeof(uint32_t);

    IngestScratch scratch;
    scratch.section = scratch_mem;
    scratch.page = scratch_mem + ZPDB_MAX_SECTION_SIZE;
    scratch.page0 = scratch_mem + 2 * ZPDB_MAX_SECTION_SIZE;
    scratch.line = (char *)(scratch_mem + 3 * ZPDB_MAX_SECTION_SIZE);
    scratch.binary = scratch_mem + 3 * ZPDB_MAX_SECTION_SIZE + ZPDB_INI_LINE_MAX;
    scratch.binary_size = 1024;

    // The store being rebuilt is the one currently open -- drop it before
    // the erase so nothing can read half-erased flash.
    g_store.close();

    if (!zpdbFlashErase())
    {
        logmsg("---- ZPDB: erase failed, the profile store is now unusable");
        return;
    }

    ZpdbWriter writer(scratch.section, scratch.page, scratch.page0);
    writer.begin(zpdbFlashProgramPage, nullptr, zpdbFlashSize(), zpdbFlashRead);

    uint32_t hash_count = 0;
    uint32_t loaded = 0, failed = 0;
    char name[ZPDB_MAX_NAME];

    while (nextProfileFile(name, sizeof(name)))
    {
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", ZPDB_PROFILE_DIR, name);

        FsFile file;
        bool ok = false;

        if (!file.open(path, O_RDONLY))
        {
            dbgmsg("---- ZPDB: could not open ", path);
        }
        else
        {
            ok = ingestFile(file, name, writer, scratch, hashes, &hash_count);
            file.close();
        }

        moveProfileFile(name, ok);
        if (ok) loaded++; else failed++;

        if (writer.failed())
        {
            // The writer latches on a flash-level failure, so nothing further
            // can be stored -- move the rest to /failed rather than looping.
            dbgmsg("---- ZPDB: the store write failed, remaining files will be skipped");
            while (nextProfileFile(name, sizeof(name)))
            {
                moveProfileFile(name, false);
                failed++;
            }
            break;
        }
    }

    if (writer.failed() || !writer.finish())
    {
        logmsg("---- ZPDB: error occured attempting to write the profile store, retry with debug on to see further error messages");
    }
    else
    {
        logmsg("-- ZPDB: stored ", (int)writer.sectionCount(), " profile(s), ",
               (int)writer.bytesWritten(), " bytes; ", (int)loaded, " file(s) loaded, ",
               (int)failed, " failed");
    }

}

// ---------------------------------------------------------------- public

// With [SCSI] Debug=1, name every profile the store holds. Each line costs a
// short read from flash for the name, so the whole walk is skipped outright
// when debug logging is off rather than left to dbgmsg() to discard.
static void logStoredProfiles()
{
    if (!g_log_debug)
        return;

    char name[ZPDB_MAX_NAME];
    zpdb_section_ref_t section;
    uint32_t index = 0;

    if (!g_store.firstSection(&section))
        return;

    do
    {
        if (!g_store.sectionName(section, name, sizeof(name)))
        {
            dbgmsg("---- ZPDB: profile ", (int)index, " has an unreadable name");
            break;
        }

        dbgmsg("---- ZPDB: profile ", (int)index, ": '", name, "' -- ",
               (int)section.entry_count, " key(s), ", (int)section.sect_len,
               " bytes at store offset ", (int)section.offset);
        index++;
        platform_poll();
    } while (g_store.nextSection(&section));
}

void zpdbProfilesInit(uint8_t *scratch, size_t scratch_size)
{
    zpdbUnbindAll();
    g_store.close();

    if (!zpdbFlashInit())
        return;

    // Opening the store happens on every call: it lives in flash and owes
    // nothing to the card. Ingesting is the part that needs the caller's
    // buffer, so no buffer means no ingest -- see the header for why the
    // caller is the one that decides.
    const bool can_ingest = (scratch != nullptr && scratch_size >= ZPDB_REBUILD_SCRATCH_SIZE);

    if (scratch != nullptr && !can_ingest)
    {
        dbgmsg("-- ZPDB: ", (int)scratch_size, " bytes of scratch were offered but a rebuild "
               "needs ", (int)ZPDB_REBUILD_SCRATCH_SIZE, ", not ingesting ", ZPDB_PROFILE_DIR);
    }

    uint32_t file_count = 0;
    if (can_ingest)
    {
        file_count = countProfileFiles();
        if (file_count > 0)
        {
            rebuildStore(file_count, scratch);
        }
    }

    if (!g_store.open(zpdbFlashRead, nullptr, zpdbFlashSize()))
    {
        dbgmsg("-- No ZuluSCSI Profile Database (ZPDB) found in Flash");
        if (!can_ingest)
        {
            // Nothing was scanned, so say only what is actually known.
            dbgmsg("-- ZPDB: no profile store in flash");
        }
        else if (file_count == 0)
        {
            dbgmsg("-- ZPDB: no profile store in flash and no .ini files in ",
                   ZPDB_PROFILE_DIR);
        }
        return;
    }

    // The store is only ever erased and rewritten whole, so what matters for
    // "will the next profile fit" is how much of the region the current store
    // leaves over -- report it rather than just the store's own size.
    uint32_t used = g_store.totalSize();
    uint32_t region = zpdbFlashSize();
    uint32_t free_bytes = (region > used) ? (region - used) : 0;
    logmsg("-- ZuluSCSI Profile Database (ZPDB) found in Flash");
    logmsg("---- ZPDB: ", (int)g_store.sectionCount(), " profile(s) in flash, using ",
           (int)used, " of ", (int)region, " bytes (", (int)((used * 100) / region),
           "%), ", (int)free_bytes, " bytes free");

    if (free_bytes < ZPDB_MAX_SECTION_SIZE)
    {
        dbgmsg("---- ZPDB: WARNING: nearing end of usable flash, ", free_bytes, " bytes left.");
    }

    logStoredProfiles();
}

bool zpdbProfilesAvailable()
{
    return g_store.isOpen();
}

uint32_t zpdbProfileCount()
{
    return g_store.sectionCount();
}

void zpdbUnbindAll()
{
    memset(g_bound, 0, sizeof(g_bound));
}

bool zpdbBindProfile(uint8_t scsiId, const char *profileName)
{
    uint8_t id = scsiId & S2S_CFG_TARGET_ID_BITS;

    if (!g_store.isOpen())
    {
        logmsg("---- ZPDB: profile '", profileName, "' requested for SCSI ID ", (int)id,
               " but no profile store is loaded");
        return false;
    }

    if (!g_store.findSection(profileName, &g_bound[id].section))
    {
        logmsg("---- ZPDB: profile '", profileName, "' was not found in the store for "
               "SCSI ID ", (int)id);
        return false;
    }

    g_bound[id].bound = true;

    logmsg("---- ZPDB: SCSI ID ", (int)id, " uses profile '", profileName, "'");
    return true;
}

bool zpdbHasProfile(uint8_t scsiId)
{
    return g_bound[scsiId & S2S_CFG_TARGET_ID_BITS].bound;
}

const char *zpdbProfileName(uint8_t scsiId)
{
    // Read back from flash rather than held per target: this is only used for
    // logging and status display, and a per-target copy of the name would cost
    // several times what the binding itself does.
    static char name[ZPDB_MAX_NAME];
    uint8_t id = scsiId & S2S_CFG_TARGET_ID_BITS;

    if (!g_bound[id].bound || !g_store.sectionName(g_bound[id].section, name, sizeof(name)))
        return nullptr;

    return name;
}

// Read a key of the bound profile straight into the caller's buffer.
static bool readKey(uint8_t scsiId, const char *key, uint8_t *buf, uint32_t buf_size,
                    uint32_t *length)
{
    uint8_t id = scsiId & S2S_CFG_TARGET_ID_BITS;

    if (!g_bound[id].bound || !g_store.isOpen())
        return false;

    zpdb_entry_ref_t entry;
    if (!g_store.findKey(g_bound[id].section, key, &entry))
        return false;

    return g_store.readValue(entry, buf, buf_size, length);
}

bool zpdbReadVPD(uint8_t scsiId, uint8_t pageCode, uint8_t *buf, uint32_t buf_size,
                 uint32_t *length)
{
    uint8_t id = scsiId & S2S_CFG_TARGET_ID_BITS;

    if (!g_bound[id].bound || !g_store.isOpen())
        return false;

    zpdb_entry_ref_t entry;
    if (!g_store.findVPD(g_bound[id].section, pageCode, &entry))
        return false;

    return g_store.readValue(entry, buf, buf_size, length);
}

bool zpdbHasVPD(uint8_t scsiId, uint8_t pageCode)
{
    uint8_t id = scsiId & S2S_CFG_TARGET_ID_BITS;

    if (!g_bound[id].bound || !g_store.isOpen())
        return false;

    zpdb_entry_ref_t entry;
    return g_store.findVPD(g_bound[id].section, pageCode, &entry);
}

bool zpdbReadSPD(uint8_t scsiId, uint8_t *buf, uint32_t buf_size, uint32_t *length)
{
    return readKey(scsiId, "SPD", buf, buf_size, length);
}

bool zpdbReadModeSense(uint8_t scsiId, uint8_t *buf, uint32_t buf_size, uint32_t *length)
{
    return readKey(scsiId, "ModeSense3F", buf, buf_size, length);
}

bool zpdbReadLogSense(uint8_t scsiId, uint8_t pageCode, uint8_t *buf, uint32_t buf_size,
                      uint32_t *length)
{
    static const char hex[] = "0123456789ABCDEF";
    char key[12] = { 'L', 'o', 'g', 'S', 'e', 'n', 's', 'e',
                     hex[pageCode >> 4], hex[pageCode & 0x0F], '\0' };
    return readKey(scsiId, key, buf, buf_size, length);
}

static bool readCapacityFromSection(zpdb_section_ref_t section, uint32_t *blockSize,
                                    uint64_t *sectors)
{
    zpdb_entry_ref_t entry;
    bool found = false;

    if (blockSize != nullptr && g_store.findKey(section, "BlockSize", &entry))
    {
        *blockSize = (uint32_t)g_store.readLong(entry, 0);
        found = true;
    }

    if (sectors != nullptr && g_store.findKey(section, "Sectors", &entry))
    {
        *sectors = g_store.readU64(entry, 0);
        found = true;
    }

    return found;
}

bool zpdbReadCapacity(uint8_t scsiId, uint32_t *blockSize, uint64_t *sectors)
{
    uint8_t id = scsiId & S2S_CFG_TARGET_ID_BITS;

    if (!g_bound[id].bound || !g_store.isOpen())
        return false;

    return readCapacityFromSection(g_bound[id].section, blockSize, sectors);
}

bool zpdbReadCapacityByName(const char *profileName, uint32_t *blockSize, uint64_t *sectors)
{
    zpdb_section_ref_t section;

    if (!g_store.isOpen() || !g_store.findSection(profileName, &section))
        return false;

    return readCapacityFromSection(section, blockSize, sectors);
}
