/**
 * Copyright (C) 2025-2026 Kevin Moonlight <me@yyzkevin.com>
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

// Custom SCSI inquiry data (VPD/SPD) from INI configuration

#include "custom_vendor_inquiry.h"

#include "ZuluSCSI_log.h"
#include "ZuluSCSI_config.h"
#include "ZuluSCSI_settings.h"
#include "ZuluSCSI_disk.h"
#include <ZuluSCSI_platform_config.h>
#ifdef PLATFORM_AS400
# include "as400_values.h"
# include "as400_tape_values.h"
#endif

#include <scsi.h>
#include <minIni.h>
#include <string.h>
#include <stdlib.h>

// Storage for custom VPD pages: entries shared across all SCSI IDs (not
// per-device -- an ID with more declared pages than another just uses more
// of the shared pool). Each entry: [0]=scsiId, [1]=pageCode, [2]=length, [3..]=data
//
// MAX_CUSTOM_VPD_ENTRIES is sized per S2S_MAX_TARGETS rather than a flat
// guess: real AS/400 disk-profile captures declare up to ~13 VPD pages each
// (see as400_disk_definitions.txt), so a flat 16-entry table -- fine for a
// single profiled ID -- silently overflows with just 2-3 profiled IDs
// loaded at once. Adding the PPC tape identity (loadAS400TapeDefaults(), up
// to 14 pages of its own, same shared pool) fits comfortably within this
// same formula-based sizing -- the most ordinary combined config (one
// AS/400 disk on its default identity plus one PPC tape unit) needs up to
// 22 entries, well under the default's 96 on a board with no override.
//
// The multiplier is deliberately modest (6, not enough for every possible
// SCSI ID to carry a full 13-page profile simultaneously) rather than a
// larger "cover every worst case" number: each entry costs 258 bytes
// (see MAX_VPD_DATA_SIZE below), and this table is static RAM on a
// platform with no swap. S2S_MAX_TARGETS*10 (160 entries on ZuluSCSI_Wide,
// ~41.3KB) was tried and overflowed the linker's fixed 512KB RAM region by
// 544 bytes on real hardware -- this firmware's other features (FreeRTOS,
// lwIP, display/UI, USB, WebUI, audio) already consume most of the ~36.6KB
// that was actually free before this table grew. *10 was sized from a
// percentage of total RAM without accounting for that; *6 (96 entries,
// ~24.8KB on Wide, a ~20.6KB growth) leaves real margin instead of just
// barely fitting. Covers ~7 fully-profiled SCSI IDs at once, comfortably
// above the "3+ IDs" scenario this fix was written to support.
//
// *6 assumed RAM headroom tracks S2S_MAX_TARGETS, which turned out false
// too: ZuluSCSI_Blaster is an 8-target board (same class as plain RP2040
// boards, which build fine at 48 entries) but also carries the full
// networking stack, DaynaPORT, audio, display/UI, and the logic sniffer on
// top -- far less headroom than its target count alone would suggest, and
// it overflowed the same 512KB RAM region by 6516 bytes at 48 entries on
// real CI. Board RAM headroom depends on each board's whole feature set,
// not just its bus width, so a single S2S_MAX_TARGETS-scaled formula can't
// fit every board -- overridable per-board via a build flag (see
// ZuluSCSI_Blaster's build_flags in platformio.ini), same pattern already
// used for PREFETCH_BUFFER_SIZE. (This branch originally carried its own,
// smaller, non-overridable literal here (32) to avoid a rebase collision
// with the disk-profile-loader branch's independent, larger fix to this
// same constant -- now that both are combined, the formula-based value
// above supersedes it, since it already exceeds what tape alone needs.)
//
// MAX_VPD_DATA_SIZE is 255 -- the maximum representable in the `length`
// field below (uint8_t). The largest AS/400 disk-profile capture in tree
// today is XCPR036 page 0xC3 at 250 bytes; pages 0xD1 / 0xD2 are 244 B.
// Going to 255 leaves a few bytes of headroom without widening `length`.
#ifndef MAX_CUSTOM_VPD_ENTRIES
#define MAX_CUSTOM_VPD_ENTRIES (S2S_MAX_TARGETS * 6)
#endif
#define MAX_VPD_DATA_SIZE 255
static struct {
    uint8_t scsiId;
    uint8_t pageCode;
    uint8_t length;
    uint8_t data[MAX_VPD_DATA_SIZE];
} g_custom_vpd[MAX_CUSTOM_VPD_ENTRIES];
static int g_custom_vpd_count = 0;

// Storage for custom standard inquiry data per SCSI ID
//
// MAX_SPD_SIZE is sized to fit the AS/400 standard INQUIRY captures
// (DGVS09U and XCPR036 are both 164 bytes) with headroom.
#define MAX_SPD_SIZE 192
static struct {
    uint8_t length;
    uint8_t data[MAX_SPD_SIZE];
} g_custom_spd[S2S_MAX_TARGETS];

// Storage for a custom MODE SENSE page 0x3F ("all pages") response per SCSI ID.
//
// MAX_MODESENSE_SIZE matches MAX_VPD_DATA_SIZE: the firmware's built-in
// as400_mode_sense_all_pages blob is 220 bytes, and the extractor script
// captures MODE SENSE(6) with an allocation length of 0xFF (255).
#define MAX_MODESENSE_SIZE 255
static struct {
    uint16_t length;
    uint8_t data[MAX_MODESENSE_SIZE];
} g_custom_modesense[S2S_MAX_TARGETS];

#ifdef PLATFORM_AS400
// Per-SCSI-ID override for the 8-byte AS/400 serial, supplied via the
// `AS400_DiskSerialNumber` key in [SCSI<n>] sections. When length == 8,
// injectSerial() uses this value instead of the SD CID / MCU-derived default.
static struct {
    uint8_t length;
    uint8_t data[8];
} g_as400_serial_override[S2S_MAX_TARGETS];

// Per-SCSI-ID override for the 7-character IBM disk part number (FRU)
// embedded in VPD page 0x01 at ASCII offset 5 and EBCDIC offset 29.
// Supplied via the `AS400_DiskPartNumber` key in [SCSI<n>] sections.
// When length == 7, injectPartNumber() patches both ASCII and EBCDIC slots.
static struct {
    uint8_t length;
    uint8_t ascii[7];
    uint8_t ebcdic[7];
} g_as400_part_override[S2S_MAX_TARGETS];

// BlockSize/Sectors captured from a loaded AS/400 disk profile (see
// loadAS400ProfileFromFile() below), for a given SCSI ID. blockSize/sectors
// are not consumed by anything yet -- live capacity reporting is always
// computed from the actual backing image file's size, never from this. They
// are here for the upcoming auto-image-creation feature, which needs to know
// a profile's real capacity before it can create a correctly-sized image for
// it. `loaded` is consumed immediately, by loadAS400Defaults() below: a
// named profile's declared VPD page set is authoritative for that SCSI ID,
// so any page IT doesn't have should stay absent rather than being patched
// in from the built-in default's unrelated physical drive.
static struct {
    uint32_t blockSize;
    uint32_t sectors;
    bool loaded;
} g_as400_profile_info[S2S_MAX_TARGETS];

// Convert a single ASCII character to IBM EBCDIC (CP037 subset).
// Supports digits, uppercase A-Z, and space. Lowercase is uppercased first.
// Anything else returns EBCDIC space (0x40).
static uint8_t asciiToEbcdic(char c)
{
    if (c >= 'a' && c <= 'z') c -= ('a' - 'A');
    if (c >= '0' && c <= '9') return (uint8_t)(0xF0 + (c - '0'));
    if (c >= 'A' && c <= 'I') return (uint8_t)(0xC1 + (c - 'A'));
    if (c >= 'J' && c <= 'R') return (uint8_t)(0xD1 + (c - 'J'));
    if (c >= 'S' && c <= 'Z') return (uint8_t)(0xE2 + (c - 'S'));
    return 0x40;
}
#endif

// Parse space/comma-separated hex values from a string into a byte buffer.
// Returns number of bytes parsed.
static int parseHexString(const char *str, uint8_t *buf, int maxlen)
{
    const char *ptr = str;
    char *end;
    int count = 0;

    while (*ptr != '\0' && count < maxlen)
    {
        buf[count++] = (uint8_t)strtol(ptr, &end, 16);
        if (ptr == end) break; // No conversion possible
        ptr = end;
        while (*ptr == ' ' || *ptr == ',') ptr++;
    }
    return count;
}

// Check if a custom VPD page already exists for a given SCSI ID and page code
static bool hasCustomVPD(uint8_t scsiId, uint8_t pageCode)
{
    for (int i = 0; i < g_custom_vpd_count; i++)
    {
        if (g_custom_vpd[i].scsiId == scsiId && g_custom_vpd[i].pageCode == pageCode)
            return true;
    }
    return false;
}

// Index of an already-loaded custom VPD page for a given SCSI ID and page
// code, for in-place mutation (unlike getCustomVPD(), which copies out).
// `startIdx` restricts the search to entries at or after that index --
// used to distinguish pages a profile just loaded from pages that already
// existed beforehand via an explicit [SCSI<n>] vpdXX= override, which must
// never be touched here (it's meant to be the final, most-authoritative
// word on that page's bytes). Returns -1 if not found.
static int findCustomVPDIndex(uint8_t scsiId, uint8_t pageCode, int startIdx = 0)
{
    for (int i = startIdx; i < g_custom_vpd_count; i++)
    {
        if (g_custom_vpd[i].scsiId == scsiId && g_custom_vpd[i].pageCode == pageCode)
            return i;
    }
    return -1;
}

#ifdef PLATFORM_AS400
// Inject the generated serial number into a VPD page at the given offset.
// Pass ebcdic=true for a slot documented as carrying an EBCDIC copy (e.g.
// VPD82 offset 38) -- the serial digits are converted via asciiToEbcdic()
// rather than copied verbatim, which is what an ASCII slot needs instead.
static void injectSerial(uint8_t *data, int offset, uint8_t scsiId, bool ebcdic = false)
{
    uint8_t serial[8];
    char string[9] = {0};

    uint8_t id = scsiId & S2S_CFG_TARGET_ID_BITS;
    if (g_as400_serial_override[id].length == 8)
    {
        memcpy(serial, g_as400_serial_override[id].data, 8);
    }
    else
    {
        as400_get_serial_8(scsiId, serial);
    }

    if (ebcdic)
    {
        for (int i = 0; i < 8; i++)
            data[offset + i] = asciiToEbcdic((char)serial[i]);
    }
    else
    {
        memcpy(data + offset, serial, 8);
    }
    memcpy(string, serial, 8);
}

// Inject the configured 7-char IBM disk part number (FRU) into the ASCII slot
// at `asciiOffset`, and optionally into an EBCDIC slot at `ebcdicOffset`. Pass
// a negative `ebcdicOffset` when the target buffer carries only an ASCII copy
// (e.g. the standard INQUIRY response). No-op when no override is configured
// for this SCSI ID.
static void injectPartNumber(uint8_t *data, int asciiOffset, int ebcdicOffset, uint8_t scsiId)
{
    uint8_t id = scsiId & S2S_CFG_TARGET_ID_BITS;
    if (g_as400_part_override[id].length != 7) return;

    memcpy(data + asciiOffset, g_as400_part_override[id].ascii, 7);
    if (ebcdicOffset >= 0)
        memcpy(data + ebcdicOffset, g_as400_part_override[id].ebcdic, 7);
}

// Patch a per-ID AS400_DiskSerialNumber override into an already-loaded
// AS/400 disk profile's VPD pages (see loadAS400ProfileFromFile() below).
// Unlike loadAS400Defaults()'s injectSerial() calls, which use offsets
// hardcoded for the ONE built-in profile's own known page layout, this
// works on ANY captured profile by deriving each page's injection offset
// from the page's own self-reported length/descriptor-length bytes --
// verified generic across every profile in as400_disk_definitions.txt as
// of 2026-09-12 (see project memory:
// project_as400_serial_collision_investigation.md). No-op if no
// AS400_DiskSerialNumber override is configured for this ID.
//
// `startIdx` must be the value of `g_custom_vpd_count` from *before* the
// profile started loading its pages -- restricts every lookup here to
// pages the profile itself just added, so an explicit [SCSI<n>] vpdXX=
// override (parsed earlier, always at a lower index) is never touched.
// That key is documented as the final, most-authoritative word on a
// page's bytes; silently patching serial bytes into a hand-crafted
// override the user typed in themselves would violate that.
static void injectSerialIntoLoadedProfile(uint8_t scsiId, int startIdx, bool spdWasEmpty)
{
    uint8_t id = scsiId & S2S_CFG_TARGET_ID_BITS;
    if (g_as400_serial_override[id].length != 8) return;

    // SPD (standard INQUIRY response) patching at offset 36 -- structurally
    // guaranteed by the SCSI-2 INQUIRY format (8-byte header + 8-byte
    // Vendor ID + 16-byte Product ID + 4-byte Revision, all fixed-width),
    // and the byte-visible serial sits there in every real capture
    // checked. Hardware-confirmed (2026-09-20, CISC): whatever consumes
    // this specific field reads it as a 28-bit binary value, not free
    // text -- an unconstrained value made DST's "Display Non-Configured
    // Units" screen show a masked/invalid serial. Force the leading
    // character to '0' right here, at this SPD write only -- NOT inside
    // as400_get_serial_8() itself, which also backs LOG SENSE page 0x31
    // for every AS/400 FIXED disk regardless of override; forcing it
    // there broke real PPC load-source recognition (SRC B1014504) the
    // first time this was tried, since page 0x31 does not share this
    // constraint.
    if (spdWasEmpty && g_custom_spd[scsiId].length >= 44)
    {
        injectSerial(g_custom_spd[scsiId].data, 36, scsiId);
        g_custom_spd[scsiId].data[36] = '0';
        logmsg("---- Patched custom serial into SPD for SCSI ID ", (int)scsiId, " at offset 36");
    }
    else if (spdWasEmpty && g_custom_spd[scsiId].length > 0)
    {
        logmsg("---- WARNING: SPD for SCSI ID ", (int)scsiId, " is only ",
               (int)g_custom_spd[scsiId].length, " bytes -- too short to patch, serial override not applied to SPD");
    }

    // VPD80 (Unit Serial Number): the real captured field width varies
    // (8 or 10 ASCII characters observed so far, always right-justified,
    // space/zero-padded on the left), but the actual per-drive-varying
    // digits are always the LAST 8 bytes of the page -- confirmed across
    // every VPD80 capture in the definitions file regardless of its
    // declared length (16 or 20 bytes seen so far). Requires at least 12
    // bytes total (4-byte page header + >=8 payload) so the write can
    // never reach into the header itself.
    int idx = findCustomVPDIndex(scsiId, 0x80, startIdx);
    if (idx >= 0 && g_custom_vpd[idx].length >= 12)
    {
        injectSerial(g_custom_vpd[idx].data, g_custom_vpd[idx].length - 8, scsiId);
        logmsg("---- Patched custom serial into VPD80 for SCSI ID ", (int)scsiId,
               " at offset ", (int)(g_custom_vpd[idx].length - 8));
    }
    else if (idx >= 0)
    {
        logmsg("---- WARNING: VPD80 for SCSI ID ", (int)scsiId, " is only ",
               (int)g_custom_vpd[idx].length, " bytes -- too short to patch, serial override not applied to this page");
    }

    // VPD82: a fixed-format IBM page, always exactly 48 bytes of payload
    // (52 with the page header) in every real capture seen -- ASCII copy
    // of the serial at offset 14, an EBCDIC copy at offset 38. Gate
    // strictly on the expected length so an unexpected future capture
    // with a differently-shaped VPD82 gets skipped, not silently
    // corrupted.
    idx = findCustomVPDIndex(scsiId, 0x82, startIdx);
    if (idx >= 0 && g_custom_vpd[idx].length == 52)
    {
        injectSerial(g_custom_vpd[idx].data, 14, scsiId);
        injectSerial(g_custom_vpd[idx].data, 38, scsiId, true);
        logmsg("---- Patched custom serial into VPD82 (ASCII+EBCDIC) for SCSI ID ", (int)scsiId);
    }
    else if (idx >= 0)
    {
        logmsg("---- WARNING: VPD82 for SCSI ID ", (int)scsiId, " is ",
               (int)g_custom_vpd[idx].length, " bytes, not the expected 52 -- serial override not applied to this page");
    }

    // VPD83 (Device Identification): the ASCII T10-vendor-ID designator
    // shape (codeset 0x02, designator type 0x01) gets the same 8-byte
    // serial substitution as the pages above. `08K0304`/`08K0264` use a
    // binary NAA/EUI-64 designator instead (codeset 0x01, type 0x02 or
    // 0x03) -- an ASCII/EBCDIC-style substitution doesn't apply there
    // (every byte value is legal in a binary field, unlike a character
    // set), so that shape XORs the descriptor's last byte with the SCSI
    // ID instead: ID 0 keeps the captured value, every other ID lands on
    // a distinct one. Cheap and sufficient to stop two SCSI IDs sharing
    // one profile from reporting an identical logical-unit identifier (an
    // initiator seeing two targets with the same one may treat them as
    // two paths to a single device, per SPC-3 7.6.3) without needing to
    // understand IBM's opaque binary encoding at all. The per-model/
    // revision 2-character prefix seen before the ASCII serial
    // (`68`/`F8`/etc.) is preserved automatically either way, since it's
    // part of the untouched, already-captured bytes ahead of the
    // injection point. Injection offset for the ASCII case is simply the
    // descriptor's own declared length byte (data[7]): descriptor data
    // starts at a fixed buffer offset 8 (4-byte page header + 4-byte
    // descriptor header, both fixed by the SCSI spec), and the serial is
    // the descriptor's own last 8 bytes, so offset = 8 + desc_len - 8 ==
    // desc_len.
    idx = findCustomVPDIndex(scsiId, 0x83, startIdx);
    if (idx >= 0 && g_custom_vpd[idx].length >= 16)
    {
        uint8_t *data = g_custom_vpd[idx].data;
        uint8_t codeset = data[4] & 0x0F;
        uint8_t desigType = data[5] & 0x0F;
        uint8_t descLen = data[7];
        if (codeset == 0x02 && desigType == 0x01 && descLen >= 8 &&
            (8 + descLen) <= g_custom_vpd[idx].length)
        {
            injectSerial(data, descLen, scsiId);
            logmsg("---- Patched custom serial into VPD83 T10-vendor-ID designator for SCSI ID ", (int)scsiId);
        }
        else if (codeset == 0x01 && (desigType == 0x02 || desigType == 0x03) &&
                 descLen >= 1 && (8 + descLen) <= g_custom_vpd[idx].length)
        {
            data[8 + descLen - 1] ^= (uint8_t)(scsiId & S2S_CFG_TARGET_ID_BITS);
            logmsg("---- Patched VPD83 binary EUI-64/NAA designator (XOR) for SCSI ID ", (int)scsiId);
        }
        else
        {
            logmsg("---- VPD83 for SCSI ID ", (int)scsiId, " is not a recognized T10-vendor-ID/EUI-64/NAA "
                   "designator shape (codeset=", (int)codeset, " type=", (int)desigType, ") -- left untouched");
        }
    }

    logmsg("---- injectSerialIntoLoadedProfile() done for SCSI ID ", (int)scsiId,
           " (VPD80 idx=", findCustomVPDIndex(scsiId, 0x80, startIdx),
           " VPD82 idx=", findCustomVPDIndex(scsiId, 0x82, startIdx),
           " VPD83 idx=", findCustomVPDIndex(scsiId, 0x83, startIdx), ")");
}
#endif

#ifdef PLATFORM_AS400
// Read a hex-byte field from a captured AS/400 disk profile section,
// reassembling it if the extractor split it into <field>_0, <field>_1, ...
// <field>_chunks (see emit_hex_field() in utils/extract_as400_disk_data.sh --
// fields short enough to fit one INI line are stored plain under <field>).
// Returns the number of bytes decoded, 0 if the field is absent entirely.
static int readProfileHexField(const char *section, const char *field, uint8_t *buf, int maxlen)
{
    // static, not a stack local: this is called up to 255 times in a row from
    // loadAS400ProfileFromFile()'s VPD-page loop, nested several calls deep
    // inside the boot-time SCSI ID scan. A 512-byte stack local here, on top
    // of that scan's own buffers, was enough to overflow the stack on real
    // hardware (CFSR StackOverflow, RP2350, confirmed via a real crash log).
    // Safe as static: this function is only ever called sequentially, never
    // reentrantly, from this single-threaded boot-time scan.
    static char tmp[512];

    if (ini_gets(section, field, "", tmp, sizeof(tmp), AS400_PROFILES_FILE) && tmp[0] != '\0')
    {
        return parseHexString(tmp, buf, maxlen);
    }

    char chunkKey[24];
    snprintf(chunkKey, sizeof(chunkKey), "%s_chunks", field);
    long numChunks = ini_getl(section, chunkKey, 0, AS400_PROFILES_FILE);
    if (numChunks <= 0) return 0;

    int total = 0;
    for (long i = 0; i < numChunks && total < maxlen; i++)
    {
        snprintf(chunkKey, sizeof(chunkKey), "%s_%ld", field, i);
        if (!ini_gets(section, chunkKey, "", tmp, sizeof(tmp), AS400_PROFILES_FILE))
            break;
        total += parseHexString(tmp, buf + total, maxlen - total);
    }
    return total;
}

// Load a named AS/400 disk profile from AS400_PROFILES_FILE
// (as400_disk_definitions.txt, captured by utils/extract_as400_disk_data.sh)
// into this SCSI ID's custom SPD/VPD/MODE SENSE storage. Only fills in data
// not already supplied by this ID's own [SCSI<n>] vpdXX/spd keys -- those
// still take precedence, same as loadAS400Defaults() below.
//
// Fails loud rather than silently falling back to the single built-in
// profile: a missing/empty definitions file or a typo'd profile name should
// be an obvious, logged error, not a quiet switch to the wrong drive.
static void loadAS400ProfileFromFile(uint8_t scsiId, const char *profileName)
{
    // Captured before this profile adds any pages of its own, so
    // injectSerialIntoLoadedProfile() at the end of this function only
    // ever touches pages loaded from the profile itself -- never an
    // explicit [SCSI<n>] vpdXX= override, which was already parsed (and
    // would already occupy a lower index) before this function was called.
    int vpdStartIdx = g_custom_vpd_count;

    // Same idea for SPD, which has no per-entry index to compare against
    // (it's a single flat per-ID slot, not an appendable list like VPD) --
    // a plain before/after emptiness check serves the same purpose: only
    // patch it if THIS call is the one that filled it in.
    bool spdWasEmpty = (g_custom_spd[scsiId].length == 0);

    FsFile f = SD.open(AS400_PROFILES_FILE, O_RDONLY);
    bool fileUsable = f.isOpen() && f.fileSize() > 0;
    if (f.isOpen()) f.close();
    if (!fileUsable)
    {
        logmsg("---- ERROR: AS/400 disk profile '", profileName, "' requested for SCSI ID ",
               (int)scsiId, " but ", AS400_PROFILES_FILE, " is missing or empty");
        return;
    }

    if (!ini_hassection(profileName, AS400_PROFILES_FILE))
    {
        logmsg("---- ERROR: AS/400 disk profile '", profileName, "' not found in ",
               AS400_PROFILES_FILE, " for SCSI ID ", (int)scsiId);
        return;
    }

    // static for the same reason as readProfileHexField()'s tmp[] above --
    // one less sizable buffer stacked on top of an already-deep call chain.
    static uint8_t tmpbuf[MAX_MODESENSE_SIZE]; // MAX_MODESENSE_SIZE == MAX_VPD_DATA_SIZE (255)
    int len;

    if (g_custom_spd[scsiId].length == 0)
    {
        len = readProfileHexField(profileName, "SPD", g_custom_spd[scsiId].data, MAX_SPD_SIZE);
        if (len > 0) g_custom_spd[scsiId].length = len;
    }

    // Read VPD page 0x00 (the standard "supported pages" list, SPC format:
    // byte 0 periph qualifier/type, byte 1 page code, bytes 2-3 page list
    // length, bytes 4.. the actual page codes) first, and only attempt the
    // specific pages it declares - typically ~8 - instead of blindly trying
    // all 255 possible page codes. minIni has no index and re-scans the
    // whole file from the start on every single query, so trying all 255
    // costs ~247 wasted full-file scans per profile load: measured at ~9
    // seconds against a real, 400+-line as400_disk_definitions.txt on real
    // hardware, long enough to matter for AS/400 DASD-discovery timing.
    if (!hasCustomVPD(scsiId, 0x00))
    {
        len = readProfileHexField(profileName, "VPD00", tmpbuf, MAX_VPD_DATA_SIZE);
        if (len > 0 && g_custom_vpd_count < MAX_CUSTOM_VPD_ENTRIES)
        {
            int idx = g_custom_vpd_count;
            g_custom_vpd[idx].scsiId = scsiId;
            g_custom_vpd[idx].pageCode = 0x00;
            g_custom_vpd[idx].length = len;
            memcpy(g_custom_vpd[idx].data, tmpbuf, len);
            g_custom_vpd_count++;
        }
        else if (len > 0)
        {
            logmsg("---- WARNING: custom VPD table full (", MAX_CUSTOM_VPD_ENTRIES,
                   " entries), VPD page 0x00 for SCSI ID ", (int)scsiId, " was not loaded");
        }
    }

    uint8_t vpd00[MAX_VPD_DATA_SIZE];
    uint8_t vpd00_len = 0;
    getCustomVPD(scsiId, 0x00, vpd00, &vpd00_len);

    if (vpd00_len >= 4)
    {
        int declared_len = (vpd00[2] << 8) | vpd00[3];
        int available = vpd00_len - 4;
        if (declared_len > available) declared_len = available;

        int i;
        for (i = 0; i < declared_len && g_custom_vpd_count < MAX_CUSTOM_VPD_ENTRIES; i++)
        {
            int page = vpd00[4 + i];
            if (page == 0x00 || hasCustomVPD(scsiId, page))
                continue; // page 0 already handled above; others already set via [SCSI<n>] vpdXX

            char field[8];
            snprintf(field, sizeof(field), "VPD%02X", page);
            len = readProfileHexField(profileName, field, tmpbuf, MAX_VPD_DATA_SIZE);
            if (len > 0)
            {
                int idx = g_custom_vpd_count;
                g_custom_vpd[idx].scsiId = scsiId;
                g_custom_vpd[idx].pageCode = page;
                g_custom_vpd[idx].length = len;
                memcpy(g_custom_vpd[idx].data, tmpbuf, len);
                g_custom_vpd_count++;
            }
        }

        if (i < declared_len)
        {
            logmsg("---- WARNING: custom VPD table full (", MAX_CUSTOM_VPD_ENTRIES,
                   " entries), some VPD pages for SCSI ID ", (int)scsiId, " were not loaded");
        }
    }
    else
    {
        // No VPD page 0x00 ("supported pages") in this capture -- some
        // CISC-era drives (e.g. 45G9463/45G9463-1/86G9124/55F9806) genuinely
        // don't support it as a discovery mechanism, even though they still
        // carry real data on other pages (see as400_disk_definitions.txt).
        // Without VPD00 the discovery loop above never runs at all, so
        // VPD01/02/03/80/82 etc. were unreachable through AS400_DiskProfile=
        // for these profiles even though the bytes are sitting right there
        // in the file -- confirmed on real 9401-P02 hardware: a profile
        // like this loaded its SPD fine but served zero VPD pages, and IPL
        // halted early. Fall back to a small, curated list of page codes
        // actually seen across the captured dataset, instead of the full
        // 255-code brute force the VPD00 path exists specifically to avoid
        // (~9s on real hardware against a 400+-line definitions file).
        static const uint8_t curatedPages[] = {
            0x01, 0x02, 0x03, 0x80, 0x81, 0x82, 0x83,
            0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC7, 0xC8, 0xD1, 0xD2
        };
        size_t i;
        for (i = 0; i < sizeof(curatedPages) && g_custom_vpd_count < MAX_CUSTOM_VPD_ENTRIES; i++)
        {
            int page = curatedPages[i];
            if (hasCustomVPD(scsiId, page))
                continue; // already set via [SCSI<n>] vpdXX override

            char field[8];
            snprintf(field, sizeof(field), "VPD%02X", page);
            len = readProfileHexField(profileName, field, tmpbuf, MAX_VPD_DATA_SIZE);
            if (len > 0)
            {
                int idx = g_custom_vpd_count;
                g_custom_vpd[idx].scsiId = scsiId;
                g_custom_vpd[idx].pageCode = page;
                g_custom_vpd[idx].length = len;
                memcpy(g_custom_vpd[idx].data, tmpbuf, len);
                g_custom_vpd_count++;
            }
        }
        if (i < sizeof(curatedPages))
        {
            logmsg("---- WARNING: custom VPD table full (", MAX_CUSTOM_VPD_ENTRIES,
                   " entries), some VPD pages for SCSI ID ", (int)scsiId, " were not loaded");
        }
    }

    if (g_custom_modesense[scsiId].length == 0)
    {
        len = readProfileHexField(profileName, "ModeSense3F", tmpbuf, MAX_MODESENSE_SIZE);
        if (len > 0)
        {
            g_custom_modesense[scsiId].length = len;
            memcpy(g_custom_modesense[scsiId].data, tmpbuf, len);
        }
    }

    long blockSize = ini_getl(profileName, "BlockSize", 0, AS400_PROFILES_FILE);
    long sectors = ini_getl(profileName, "Sectors", 0, AS400_PROFILES_FILE);
    if (blockSize > 0) g_as400_profile_info[scsiId].blockSize = (uint32_t)blockSize;
    if (sectors > 0) g_as400_profile_info[scsiId].sectors = (uint32_t)sectors;
    g_as400_profile_info[scsiId].loaded = true;

    // Apply this ID's own AS400_DiskSerialNumber override (parsed earlier
    // in parseCustomInquiryData(), before this function runs) on top of
    // whatever the profile just supplied -- lets two SCSI IDs share the
    // same AS400_DiskProfile= without OS/400 seeing identical serials.
    // No-op if no override is configured for this ID.
    injectSerialIntoLoadedProfile(scsiId, vpdStartIdx, spdWasEmpty);

    logmsg("---- Loaded AS/400 disk profile '", profileName, "' for SCSI ID ", (int)scsiId,
           " (BlockSize=", (int)blockSize, " Sectors=", (int)sectors, ")");
}
#endif

#ifdef PLATFORM_AS400
// Populate default AS/400 inquiry and VPD data
// Only fills in data that wasn't already provided via INI.
static void loadAS400Defaults(uint8_t scsiId,S2S_CFG_TYPE type)
{
    bool loaded_default_data = false;

    if (!((g_scsi_settings.getSystem()->quirks & S2S_CFG_QUIRKS_AS400) && type== S2S_CFG_FIXED))
        return;

        // Default standard inquiry (SPD) with serial and part number injected.
    // The SPD carries only an ASCII copy of the 7-char IBM disk part number
    // at offsets 114-120 — there is no EBCDIC slot here, unlike VPD page 0x01.
    if (g_custom_spd[scsiId].length == 0)
    {
        size_t len = AS400VendorInquiryLen;
        if (len > MAX_SPD_SIZE) len = MAX_SPD_SIZE;
        memcpy(g_custom_spd[scsiId].data, AS400VendorInquiry, len);
        // Patch vendor (bytes 8-15) and product ID (bytes 16-31) from device
        // settings so that [SCSI<X>] and [SCSIn] Vendor/Product overrides take
        // effect even when the AS/400 default SPD is active.
        const scsi_device_settings_t *devCfg = g_scsi_settings.getDevice(scsiId);
        if (len >= 16)
            memcpy(g_custom_spd[scsiId].data + 8, devCfg->vendor, sizeof(devCfg->vendor));
        if (len >= 32)
            memcpy(g_custom_spd[scsiId].data + 16, devCfg->prodId, sizeof(devCfg->prodId));
        if (len >= 46)
        {
            // Same 28-bit-value SPD constraint as
            // injectSerialIntoLoadedProfile()'s named-profile path above --
            // force the leading character here at the SPD write only, not
            // inside as400_get_serial_8() itself. See that function's own
            // comment for why: it also backs LOG SENSE page 0x31, which
            // does not share this constraint and broke on real PPC
            // hardware when this was forced there instead.
            injectSerial(g_custom_spd[scsiId].data, 38, scsiId);
            g_custom_spd[scsiId].data[38] = '0';
        }
        if (len >= 121)
            injectPartNumber(g_custom_spd[scsiId].data, 114, -1, scsiId);
        g_custom_spd[scsiId].length = len;
        loaded_default_data = true;
    }

    // Default VPD pages. Skipped entirely once a named profile is active for
    // this ID (see loadAS400ProfileFromFile()): that profile's own captured
    // page set is authoritative, and patching in a page it doesn't have from
    // the built-in default would splice a different, unrelated physical
    // drive's identity data into an otherwise self-consistent profile -
    // confirmed in practice for VPD pages 0x01/0x82/0x83 on a profile that
    // doesn't happen to capture them.
    size_t p;
    for (p = 0; p < AS400VitalPagesLen && g_custom_vpd_count < MAX_CUSTOM_VPD_ENTRIES; p++)
    {
        uint8_t pageLen = AS400VitalPages[p][0]; // first byte is length
        if (pageLen < 2) continue;
        uint8_t pageCode = AS400VitalPages[p][2]; // page code at offset 2 in data

        if (hasCustomVPD(scsiId, pageCode) || g_as400_profile_info[scsiId].loaded)
            continue; // INI override, or an active named profile, takes precedence

        loaded_default_data = true;
        int idx = g_custom_vpd_count;
        g_custom_vpd[idx].scsiId = scsiId;
        g_custom_vpd[idx].pageCode = pageCode;
        g_custom_vpd[idx].length = pageLen;
        if (pageLen > MAX_VPD_DATA_SIZE) g_custom_vpd[idx].length = MAX_VPD_DATA_SIZE;
        memcpy(g_custom_vpd[idx].data, &AS400VitalPages[p][1], g_custom_vpd[idx].length);

        // Inject serial into pages that contain it
        if (pageCode == 0x80 && g_custom_vpd[idx].length >= 20)
            injectSerial(g_custom_vpd[idx].data, 12, scsiId); // offset 12 in page data
        else if (pageCode == 0x82 && g_custom_vpd[idx].length >= 24)
        {
            // Offset 14, not 16 -- landmark-verified (search for the "IBM"
            // string terminator, read the 8 bytes before it) across 7
            // independently captured real drives (59H7001, 59H6611,
            // 9V8006-041, 86G9124, 55F9806, 45G9463, 45G9463-1), spanning
            // multiple product families. The shipped offset of 16 was off
            // by 2 relative to every real drive checked.
            injectSerial(g_custom_vpd[idx].data, 14, scsiId);
            // VPD82's vendor-specific area also carries an EBCDIC copy of
            // the same serial (SCSI-2 8.3.4.1 table 103 defines the
            // structure, not IBM's use of it) -- offset 38, same as
            // injectSerialIntoLoadedProfile()'s named-profile path above,
            // verified against every VPD82 capture in
            // as400_disk_definitions.txt. Left unpatched here previously:
            // this default (no AS400_DiskProfile=) identity path shares
            // the built-in 09L4044 identity across every SCSI ID that
            // falls back to it, so its own VPD82 needs the same ASCII+
            // EBCDIC treatment to stay internally consistent.
            if (g_custom_vpd[idx].length >= 46)
                injectSerial(g_custom_vpd[idx].data, 38, scsiId, true);
        }
        else if (pageCode == 0x83 && g_custom_vpd[idx].length >= 42)
            injectSerial(g_custom_vpd[idx].data, 34, scsiId);
        else if (pageCode == 0xD1 && g_custom_vpd[idx].length >= 78)
            injectSerial(g_custom_vpd[idx].data, 70, scsiId);

        // Inject configured IBM disk part number (FRU) into VPD page 0x01.
        // ASCII slot at offset 5 and EBCDIC slot at offset 29, 7 bytes each.
        if (pageCode == 0x01 && g_custom_vpd[idx].length >= 36)
            injectPartNumber(g_custom_vpd[idx].data, 5, 29, scsiId);

        g_custom_vpd_count++;
    }
    if (p < AS400VitalPagesLen)
    {
        logmsg("---- WARNING: custom VPD table full (", MAX_CUSTOM_VPD_ENTRIES,
               " entries), some default VPD pages for SCSI ID ", (int)scsiId, " were not loaded");
    }
    if (loaded_default_data)
    {
        logmsg("---- Loaded default AS/400 inquiry data for SCSI ID ", (int) scsiId);
    }
}

// Populate default AS/400 tape identity data (real captures from a real
// CISC-era and a real PPC-era tape drive, see as400_tape_values.h). Kept
// separate from loadAS400Defaults() above rather than folding tape branches
// into it -- the data shape and injection semantics differ enough (no
// per-page serial injection for tape; the CISC variant has no VPD at all)
// that sharing the function would risk the disk path for no benefit. Only
// fills in data that wasn't already provided via INI (mirrors
// loadAS400Defaults()'s own precedence rules).
static void loadAS400TapeDefaults(uint8_t scsiId, S2S_CFG_TYPE type)
{
    if (!((g_scsi_settings.getSystem()->quirks & S2S_CFG_QUIRKS_AS400) && type == S2S_CFG_SEQUENTIAL))
        return;

    scsi_device_preset_t preset = g_scsi_settings.getDevicePreset(scsiId);

    const uint8_t *inquiry = nullptr; size_t inquiryLen = 0;
    const uint8_t *modeSense = nullptr; size_t modeSenseLen = 0;
    const uint8_t (*vitalPages)[255] = nullptr; size_t vitalPagesLen = 0;
    const char *presetName = nullptr;

    switch (preset)
    {
        case DEV_PRESET_AS400_BS520: [[fallthrough]];
        case DEV_PRESET_AS400_CISC:
            inquiry = AS400TapeCISCVendorInquiry; inquiryLen = AS400TapeCISCVendorInquiryLen;
            modeSense = as400_tape_cisc_mode_sense_all_pages; modeSenseLen = as400_tape_cisc_mode_sense_all_pagesLen;
            // No VPD table -- the real captured CISC drive doesn't support VPD/EVPD at all.
            presetName = "CISC";
            break;
        case DEV_PRESET_AS400_BS522: [[fallthrough]];
        case DEV_PRESET_AS400_PPC:
            inquiry = AS400TapePPCVendorInquiry; inquiryLen = AS400TapePPCVendorInquiryLen;
            modeSense = as400_tape_ppc_mode_sense_all_pages; modeSenseLen = as400_tape_ppc_mode_sense_all_pagesLen;
            vitalPages = AS400TapePPCVitalPages; vitalPagesLen = AS400TapePPCVitalPagesLen;
            presetName = "PPC";
            break;
        default:
            logmsg("---- AS/400 tape quirk active for SCSI ID ", (int)scsiId,
                   " but no Device=AS400_CISC/AS400_PPC set for this ID -- leaving generic tape identity");
            return;
    }

    bool loaded_default_data = false;

    if (g_custom_spd[scsiId].length == 0)
    {
        size_t len = inquiryLen;
        if (len > MAX_SPD_SIZE) len = MAX_SPD_SIZE;
        memcpy(g_custom_spd[scsiId].data, inquiry, len);
        g_custom_spd[scsiId].length = len;
        loaded_default_data = true;
    }

    if (g_custom_modesense[scsiId].length == 0)
    {
        size_t len = modeSenseLen;
        if (len > MAX_MODESENSE_SIZE) len = MAX_MODESENSE_SIZE;
        memcpy(g_custom_modesense[scsiId].data, modeSense, len);
        g_custom_modesense[scsiId].length = len;
        loaded_default_data = true;
    }

    if (vitalPages != nullptr)
    {
        size_t p;
        for (p = 0; p < vitalPagesLen && g_custom_vpd_count < MAX_CUSTOM_VPD_ENTRIES; p++)
        {
            uint8_t pageLen = vitalPages[p][0]; // first byte is length
            if (pageLen < 2) continue;
            uint8_t pageCode = vitalPages[p][2]; // page code at offset 2 in data

            if (hasCustomVPD(scsiId, pageCode))
                continue; // an explicit [SCSI<n>] vpdXX override takes precedence

            loaded_default_data = true;
            int idx = g_custom_vpd_count;
            g_custom_vpd[idx].scsiId = scsiId;
            g_custom_vpd[idx].pageCode = pageCode;
            g_custom_vpd[idx].length = pageLen;
            if (pageLen > MAX_VPD_DATA_SIZE) g_custom_vpd[idx].length = MAX_VPD_DATA_SIZE;
            memcpy(g_custom_vpd[idx].data, &vitalPages[p][1], g_custom_vpd[idx].length);
            g_custom_vpd_count++;
        }
        if (p < vitalPagesLen)
        {
            logmsg("---- WARNING: custom VPD table full (", MAX_CUSTOM_VPD_ENTRIES,
                   " entries), some default tape VPD pages for SCSI ID ", (int)scsiId, " were not loaded");
        }
    }

    if (loaded_default_data)
    {
        logmsg("---- Loaded default AS/400 ", presetName, " tape inquiry data for SCSI ID ", (int)scsiId);
    }
}
#endif

// Resets shared storage for ALL SCSI IDs. Must be called exactly once before
// the scan loop that calls parseCustomInquiryData() once per discovered ID --
// previously these resets lived at the top of parseCustomInquiryData() itself,
// which meant every ID's call wiped every *other* already-processed ID's
// custom VPD/SPD/serial/part-number data. Only the last ID scanned ever kept
// its custom data. Not previously visible because nothing exercised custom
// data differing across multiple IDs at once.
void resetCustomInquiryData()
{
    g_custom_vpd_count = 0;
    memset(g_custom_spd, 0, sizeof(g_custom_spd));
    memset(g_custom_modesense, 0, sizeof(g_custom_modesense));
#ifdef PLATFORM_AS400
    memset(g_as400_serial_override, 0, sizeof(g_as400_serial_override));
    memset(g_as400_part_override, 0, sizeof(g_as400_part_override));
    memset(g_as400_profile_info, 0, sizeof(g_as400_profile_info));
#endif
}

void parseCustomInquiryData(uint8_t scsiId, S2S_CFG_TYPE type)
{
    // static, not a stack local: this function calls into
    // loadAS400ProfileFromFile() (AS400_DiskProfile=) with this buffer still
    // live on the stack, which itself nests further into
    // readProfileHexField()'s SD-card I/O -- the exact "sizable buffer
    // stacked on top of an already-deep call chain" shape that overflowed
    // the stack once already (see readProfileHexField()'s own tmp[], fixed
    // in 582e57a) -- just one frame further out, and missed by that fix.
    // Confirmed via a real crash log (CFSR StackOverflow, RP2350) with
    // AS400_DiskProfile = "45G9463" configured on the crashing ID. Safe as
    // static: this function is only ever called sequentially, never
    // reentrantly, from the single-threaded boot-time SCSI ID scan.
    static char tmp[512];
    char section[SCSI_INI_SECTION_SIZE];
    char key[8];

    scsiGetIniSection(scsiId, section, sizeof(section));

    // Parse VPD pages: vpd00, vpd80, etc.
    int page;
    for (page = 0; page < 0xFF && g_custom_vpd_count < MAX_CUSTOM_VPD_ENTRIES; page++)
    {
        snprintf(key, sizeof(key), "vpd%02x", page);
        if (ini_gets(section, key, "", tmp, sizeof(tmp), CONFIGFILE))
        {
            int idx = g_custom_vpd_count;
            g_custom_vpd[idx].scsiId = scsiId;
            g_custom_vpd[idx].pageCode = page;
            g_custom_vpd[idx].length = parseHexString(tmp, g_custom_vpd[idx].data, MAX_VPD_DATA_SIZE);
            if (g_custom_vpd[idx].length > 0)
            {
                logmsg("---- Custom VPD page 0x", key + 3, " for SCSI ID ", scsiId,
                        ": ", (int)g_custom_vpd[idx].length, " bytes");
                g_custom_vpd_count++;
            }
        }
    }

    if (page < 0xFF)
    {
        logmsg("---- WARNING: custom VPD table full (", MAX_CUSTOM_VPD_ENTRIES,
               " entries), SCSI ID ", scsiId, " vpdXX overrides for page number ",
               page, " and above were not checked");
    }

    // Parse standard inquiry override: spd=
    if (ini_gets(section, "spd", "", tmp, sizeof(tmp), CONFIGFILE))
    {
        g_custom_spd[scsiId].length = parseHexString(tmp, g_custom_spd[scsiId].data, MAX_SPD_SIZE);
        if (g_custom_spd[scsiId].length > 0)
        {
            logmsg("---- Custom SPD for SCSI ID ", scsiId, ": ", (int)g_custom_spd[scsiId].length, " bytes");
        }
    }
#ifdef PLATFORM_AS400
    // Parse AS/400 serial override: AS400_DiskSerialNumber=<up to 8 chars>
    // Shorter values are right-padded with ASCII spaces; longer values are truncated.
    if (ini_gets(section, "AS400_DiskSerialNumber", "", tmp, sizeof(tmp), CONFIGFILE))
    {
        size_t slen = strlen(tmp);
        if (slen > 0)
        {
            uint8_t id = scsiId & S2S_CFG_TARGET_ID_BITS;
            memset(g_as400_serial_override[id].data, ' ', 8);
            if (slen > 8) slen = 8;
            memcpy(g_as400_serial_override[id].data, tmp, slen);
            g_as400_serial_override[id].length = 8;
            logmsg("---- Custom AS/400 serial for SCSI ID ", (int) scsiId, ": \"", tmp, "\"");
        }
    }

    // Parse AS/400 disk part number override: AS400_DiskPartNumber=<up to 7 chars>
    // Accepts [0-9 A-Z] (lowercase is uppercased). Shorter values are right-padded
    // with spaces; longer values are truncated to 7 characters. The same 7 chars
    // are injected into both the ASCII and EBCDIC slots of VPD page 0x01.
    if (ini_gets(section, "AS400_DiskPartNumber", "", tmp, sizeof(tmp), CONFIGFILE))
    {
        size_t slen = strlen(tmp);
        if (slen > 0)
        {
            uint8_t id = scsiId & S2S_CFG_TARGET_ID_BITS;
            memset(g_as400_part_override[id].ascii, ' ', 7);
            memset(g_as400_part_override[id].ebcdic, 0x40, 7);
            if (slen > 7) slen = 7;
            for (size_t i = 0; i < slen; i++)
            {
                char c = tmp[i];
                if (c >= 'a' && c <= 'z') c -= ('a' - 'A');
                uint8_t eb = asciiToEbcdic(c);
                g_as400_part_override[id].ascii[i] = (eb == 0x40 && c != ' ') ? ' ' : (uint8_t)c;
                g_as400_part_override[id].ebcdic[i] = eb;
            }
            g_as400_part_override[id].length = 7;
            logmsg("---- Custom AS/400 disk part number for SCSI ID ", (int) scsiId, ": \"", tmp, "\"");
        }
    }

    // Load a named AS/400 disk profile: AS400_DiskProfile=<section name in
    // as400_disk_definitions.txt, e.g. "59H7001">. Runs after this section's
    // own vpdXX/spd keys above (which still win) and before the built-in
    // defaults below (which fill in anything the profile doesn't supply).
    if (ini_gets(section, "AS400_DiskProfile", "", tmp, sizeof(tmp), CONFIGFILE))
    {
        if (tmp[0] != '\0')
            loadAS400ProfileFromFile(scsiId, tmp);
    }

    // Load AS/400 defaults for any IDs that don't have INI overrides
    loadAS400Defaults(scsiId, type);
    loadAS400TapeDefaults(scsiId, type);
#endif
}

bool getCustomVPD(uint8_t scsiId, uint8_t pageCode, uint8_t *buf, uint8_t *length)
{
    for (int i = 0; i < g_custom_vpd_count; i++)
    {
        if (g_custom_vpd[i].scsiId == (scsiId & S2S_CFG_TARGET_ID_BITS) && g_custom_vpd[i].pageCode == pageCode)
        {
            *length = g_custom_vpd[i].length;
            memcpy(buf, g_custom_vpd[i].data, g_custom_vpd[i].length);
            return true;
        }
    }
    return false;
}

bool getCustomSPD(uint8_t scsiId, uint8_t *buf, uint16_t *length)
{
    uint8_t id = scsiId & S2S_CFG_TARGET_ID_BITS;
    if (g_custom_spd[id].length > 0)
    {
        *length = g_custom_spd[id].length;
        memcpy(buf, g_custom_spd[id].data, g_custom_spd[id].length);
        return true;
    }
    return false;
}

bool getCustomModeSense(uint8_t scsiId, uint8_t *buf, uint16_t *length)
{
    uint8_t id = scsiId & S2S_CFG_TARGET_ID_BITS;
    if (g_custom_modesense[id].length > 0)
    {
        *length = g_custom_modesense[id].length;
        memcpy(buf, g_custom_modesense[id].data, g_custom_modesense[id].length);
        return true;
    }
    return false;
}

#ifdef PLATFORM_AS400
bool getAS400ProfileCapacity(uint8_t scsiId, uint32_t *blockSize, uint32_t *sectors)
{
    uint8_t id = scsiId & S2S_CFG_TARGET_ID_BITS;
    if (!g_as400_profile_info[id].loaded)
        return false;

    *blockSize = g_as400_profile_info[id].blockSize;
    *sectors = g_as400_profile_info[id].sectors;
    return true;
}
#endif

