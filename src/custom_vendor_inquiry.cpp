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

#include "zpdb_profiles.h"
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

// Custom inquiry data is not cached in RAM. A page comes from one of two
// places, in this order:
//
//   1. a custom profile store, when a profile is bound to the SCSI ID -- the one
//      built from the SD card, or failing that the one compiled into the
//      firmware (see zpdb_profiles.h)
//   2. the built-in AS/400 identities, which are const arrays already in
//      flash (as400_values.h, as400_tape_values.h)
//
// Either way the bytes are copied straight into the destination buffer --
// normally scsiDev.data -- and patched there. What used to be three static
// tables holding a mutable per-target copy of every page is now the one byte
// per target below, recording which built-in identity applies.
#define MAX_VPD_DATA_SIZE 255

// Cap used when serving into the caller's buffer (scsiDev.data, at least
// 4 kB on every real board). Deliberately larger than the 255 the old static
// MODE SENSE table used: the captured MODE SENSE 0x3F responses run to 274
// bytes, which that table silently truncated.
#define ZPDB_SERVE_MAX 512

// Which built-in identity applies to a SCSI ID, chosen once when the ID is
// configured. The data itself never leaves its const array.
enum builtin_identity_t : uint8_t
{
    BUILTIN_NONE = 0,
    BUILTIN_AS400_DISK,
    BUILTIN_AS400_TAPE_CISC,
    BUILTIN_AS400_TAPE_PPC
};
static uint8_t g_builtin[S2S_MAX_TARGETS];

#ifdef PLATFORM_AS400
// Per-SCSI-ID override for the 8-byte AS/400 serial, supplied via the
// `AS400_DiskSerialNumber` key in [SCSI<X>] sections. When length == 8,
// injectSerial() uses this value instead of the SD CID / MCU-derived default.
static struct {
    uint8_t length;
    uint8_t data[8];
} g_as400_serial_override[S2S_MAX_TARGETS];

// Per-SCSI-ID override for the 7-character IBM disk part number (FRU)
// embedded in VPD page 0x01 at ASCII offset 5 and EBCDIC offset 29.
// Supplied via the `AS400_DiskPartNumber` key in [SCSI<X>] sections.
// When length == 7, injectPartNumber() patches both ASCII and EBCDIC slots.
static struct {
    uint8_t length;
    uint8_t ascii[7];
    uint8_t ebcdic[7];
} g_as400_part_override[S2S_MAX_TARGETS];

// BlockSize/Sectors captured from a bound AS/400 disk profile (see
// zpdbBindProfile(), below), for a given SCSI ID. They are read back by
// getAS400ProfileCapacity(), which auto-image-creation uses to size a new
// image for a profiled ID that has none yet; live capacity reporting is
// always computed from the actual backing image file's size, never from
// this. `loaded` is consumed by selectAS400Builtin() below: a named
// profile's declared VPD page set is authoritative for that SCSI ID, so any
// page IT doesn't have should stay absent rather than being patched in from
// the built-in default's unrelated physical drive.
static struct {
    uint32_t blockSize;
    uint32_t sectors;
    bool loaded;
} g_as400_profile_info[S2S_MAX_TARGETS];

// A page is now patched every time it is served, so a page whose shape the
// serial override cannot be applied to would otherwise log the same warning
// on every INQUIRY the host sends. One bit per page per target, so each such
// warning is said once per boot and then stays quiet.
#define PATCH_WARNED_SPD    0x01
#define PATCH_WARNED_VPD80  0x02
#define PATCH_WARNED_VPD82  0x04
#define PATCH_WARNED_VPD83  0x08
static uint8_t g_patch_warned[S2S_MAX_TARGETS];

static bool warnOnce(uint8_t scsiId, uint8_t bit)
{
    uint8_t id = scsiId & S2S_CFG_TARGET_ID_BITS;
    if (g_patch_warned[id] & bit) return false;
    g_patch_warned[id] |= bit;
    return true;
}

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


// Inject the serial number into a page at the given offset: the configured
// AS400_DiskSerialNumber when there is one, otherwise the generated default
// (which already mixes the SCSI ID in, see as400_get_serial_8()).
//
// Pass ebcdic=true for a slot documented as carrying an EBCDIC copy (e.g.
// VPD82 offset 38) -- the serial digits are converted via asciiToEbcdic()
// rather than copied verbatim, which is what an ASCII slot needs instead.
static void injectSerial(uint8_t *data, int offset, uint8_t scsiId, bool ebcdic = false)
{
    uint8_t serial[8];
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
#endif // PLATFORM_AS400


#ifdef PLATFORM_AS400
// Decide which built-in AS/400 identity a SCSI ID falls back to when the
// profile store has nothing for it. This used to copy the whole identity into
// the static tables; it now only records the choice, and the getters below
// read the const arrays directly when a page is actually asked for.
static void selectAS400Builtin(uint8_t scsiId, S2S_CFG_TYPE type)
{
    uint8_t id = scsiId & S2S_CFG_TARGET_ID_BITS;

    if (!(g_scsi_settings.getSystem()->quirks & S2S_CFG_QUIRKS_AS400))
        return;

    if (type == S2S_CFG_FIXED)
    {
        // A bound profile is authoritative for the ID: its captured page set
        // stands on its own, and filling in a page it does not have from the
        // built-in default would splice a different physical drive's identity
        // into an otherwise self-consistent profile -- confirmed in practice
        // for VPD pages 0x01/0x82/0x83.
        if (g_as400_profile_info[id].loaded)
            return;

        g_builtin[id] = BUILTIN_AS400_DISK;
        logmsg("---- Using built-in AS/400 disk inquiry data for SCSI ID ", (int)scsiId);
        return;
    }

    if (type != S2S_CFG_SEQUENTIAL)
        return;

    switch (g_scsi_settings.getDevicePreset(scsiId))
    {
        case DEV_PRESET_AS400_BS520: [[fallthrough]];
        case DEV_PRESET_AS400_CISC:
            // The real captured CISC drive does not support VPD/EVPD at all,
            // so this identity is standard INQUIRY and MODE SENSE only.
            g_builtin[id] = BUILTIN_AS400_TAPE_CISC;
            logmsg("---- Using built-in AS/400 CISC tape inquiry data for SCSI ID ",
                   (int)scsiId);
            break;

        case DEV_PRESET_AS400_BS522: [[fallthrough]];
        case DEV_PRESET_AS400_PPC:
            g_builtin[id] = BUILTIN_AS400_TAPE_PPC;
            logmsg("---- Using built-in AS/400 PPC tape inquiry data for SCSI ID ",
                   (int)scsiId);
            break;

        default:
            logmsg("---- AS/400 tape quirk active for SCSI ID ", (int)scsiId,
                   " but no Device=AS400_CISC/AS400_PPC set for this ID -- leaving generic tape identity");
            break;
    }
}

// Look up one page in a built-in vital-product-data table. Each entry is
// [0] = length, [1..] = the page itself, so the page code sits at offset 1
// of the page data.
static const uint8_t *findBuiltinVPD(const uint8_t (*pages)[255], size_t count,
                                     uint8_t pageCode, uint8_t *length)
{
    for (size_t p = 0; p < count; p++)
    {
        uint8_t pageLen = pages[p][0];
        if (pageLen < 2) continue;

        if (pages[p][2] == pageCode)
        {
            *length = (pageLen > MAX_VPD_DATA_SIZE) ? MAX_VPD_DATA_SIZE : pageLen;
            return &pages[p][1];
        }
    }
    return nullptr;
}
#endif // PLATFORM_AS400

// Resets shared storage for ALL SCSI IDs. Must be called exactly once before
// the scan loop that calls parseCustomInquiryData() once per discovered ID --
// previously these resets lived at the top of parseCustomInquiryData() itself,
// which meant every ID's call wiped every *other* already-processed ID's
// custom VPD/SPD/serial/part-number data. Only the last ID scanned ever kept
// its custom data. Not previously visible because nothing exercised custom
// data differing across multiple IDs at once.
void resetCustomInquiryData()
{
    zpdbUnbindAll();
    memset(g_builtin, 0, sizeof(g_builtin));
#ifdef PLATFORM_AS400
    memset(g_as400_serial_override, 0, sizeof(g_as400_serial_override));
    memset(g_as400_part_override, 0, sizeof(g_as400_part_override));
    memset(g_as400_profile_info, 0, sizeof(g_as400_profile_info));
    memset(g_patch_warned, 0, sizeof(g_patch_warned));
#endif
}

#ifdef PLATFORM_AS400
// Read one AS400_* key for a SCSI ID. `dynamic_section` is [SCSIn] when this
// ID is the one an SCA backplane handed out at boot, and NULL otherwise; a key
// set there wins over the same key in the ID's own [SCSI<X>] section. That is
// the precedence ZuluSCSISettings::applyDynamicSectionOverrides() already
// gives ordinary device settings, and it is what lets a card keep working in
// any backplane slot: [SCSIn] describes "whichever ID I turn out to be".
//
// Resolved per key rather than per section, so a config can keep some keys in
// [SCSIn] and the rest in [SCSI<X>] without the mere presence of [SCSIn]
// hiding the latter.
static int readAS400Key(const char *section, const char *dynamic_section,
                        const char *key, char *out, size_t outlen)
{
    if (dynamic_section != NULL)
    {
        int len = ini_gets(dynamic_section, key, "", out, outlen, CONFIGFILE);
        if (len > 0 && out[0] != '\0')
        {
            return len;
        }
    }
    return ini_gets(section, key, "", out, outlen, CONFIGFILE);
}
#endif // PLATFORM_AS400

void parseCustomInquiryData(uint8_t scsiId, S2S_CFG_TYPE type)
{
#ifdef PLATFORM_AS400
    // static, not a stack local: this runs from the boot-time SCSI ID scan,
    // on a call chain that has overflowed the stack on real hardware once
    // already with a buffer this size stacked on top of it (CFSR
    // StackOverflow, RP2350, fixed in 582e57a). Safe as static: this function
    // is only ever called sequentially, never reentrantly, from that
    // single-threaded scan.
    static char tmp[512];
    char section[SCSI_INI_SECTION_SIZE];

    scsiGetIniSection(scsiId, section, sizeof(section));

    // The run-time acquired ID also answers to [SCSIn], which is where a config
    // that has to survive being moved between backplane slots keeps its AS400_*
    // keys. readAS400Key() below lets those override this ID's own [SCSI<X>]
    // section.
    const char *dynamic_section = NULL;
#ifdef DYNAMIC_SCSI_ID
    int8_t dynamic_id = scsiDiskGetDynamicId();
    if (dynamic_id >= 0 && (scsiId & S2S_CFG_TARGET_ID_BITS) == (uint8_t)dynamic_id)
    {
        dynamic_section = DYNAMIC_SCSI_INI_SECTION;
    }
#endif // DYNAMIC_SCSI_ID

    // Parse AS/400 serial override: AS400_DiskSerialNumber=<up to 8 chars>
    // Shorter values are right-padded with ASCII spaces; longer values are truncated.
    if (readAS400Key(section, dynamic_section, "AS400_DiskSerialNumber", tmp, sizeof(tmp)))
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
    if (readAS400Key(section, dynamic_section, "AS400_DiskPartNumber", tmp, sizeof(tmp)))
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

    // Bind a named AS/400 disk profile: AS400_DiskProfile=<name of a profile,
    // e.g. "59H7001">, looked up in the custom store built from the SD card first
    // and then in the one compiled into the firmware. Binding is a linear scan of
    // a store's section headers and costs no RAM -- the pages themselves stay
    // in flash until they are served. Fails loud: an ID that names a profile
    // neither store holds is logged rather than quietly falling back to the
    // built-in identity, which would be a different drive.
    if (readAS400Key(section, dynamic_section, "AS400_DiskProfile", tmp, sizeof(tmp)))
    {
        if (tmp[0] != '\0')
        {
            if (zpdbBindProfile(scsiId, tmp))
            {
                uint8_t bound_id = scsiId & S2S_CFG_TARGET_ID_BITS;
                uint32_t blockSize = 0;
                uint64_t sectors = 0;

                if (zpdbReadCapacity(scsiId, &blockSize, &sectors))
                {
                    if (blockSize > 0) g_as400_profile_info[bound_id].blockSize = blockSize;
                    if (sectors > 0) g_as400_profile_info[bound_id].sectors = (uint32_t)sectors;
                }

                // This ID's profile is authoritative, so the built-in
                // defaults must not fill in pages the profile deliberately
                // does not have.
                g_as400_profile_info[bound_id].loaded = true;

                logmsg("---- Bound AS/400 disk profile '", tmp, "' to SCSI ID ", (int)scsiId,
                       " (BlockSize=", (int)blockSize, " Sectors=", (int)sectors, ")");
            }
            else
            {
                logmsg("---- ERROR: AS/400 disk profile ", tmp, " requested for SCSI ID ",
                       (int)scsiId, " but it is not in the custom or built-in profile store");
            }
        }
    }

    // Fall back to a built-in AS/400 identity when nothing else supplies one.
    selectAS400Builtin(scsiId, type);
#else
    (void)scsiId;
    (void)type;
#endif
}

// Identity fields are applied to the buffer the response is about to be
// served from (scsiDev.data), rather than being baked into a per-target copy
// at load time. That is what lets a profile page stay in flash: nothing needs
// a writable copy of the page just to carry a unique serial or FRU.
//
// `pageCode` is a VPD page code, or ZPDB_SERVE_SPD for the standard INQUIRY
// response.
#define ZPDB_SERVE_SPD 0x100

#ifdef PLATFORM_AS400
// Patch a per-ID AS400_DiskSerialNumber override into a page just read out of
// a bound profile in the flash store.
//
// Unlike applyBuiltinIdentityFields() below, whose offsets are hardcoded for
// the ONE built-in profile's own known page layout, this works on ANY
// captured profile by deriving each page's injection offset from the page's
// own self-reported length / descriptor-length bytes -- verified generic
// across every profile in as400_disk_definitions.txt.
//
// No-op unless an AS400_DiskSerialNumber override is configured for this ID:
// without one, a named profile's own originally-captured serial is served
// verbatim and unchanged. That is why two SCSI IDs sharing one profile need a
// distinct override on each -- see README-as400.md, "Differentiating
// same-profile disks".
static void applyProfileIdentityFields(int pageCode, uint8_t *buf, uint32_t len, uint8_t scsiId)
{
    uint8_t id = scsiId & S2S_CFG_TARGET_ID_BITS;
    if (g_as400_serial_override[id].length != 8) return;

    switch (pageCode)
    {
        // SPD (standard INQUIRY response) patching at offset 36 --
        // structurally guaranteed by the SCSI-2 INQUIRY format (8-byte
        // header + 8-byte Vendor ID + 16-byte Product ID + 4-byte Revision,
        // all fixed-width), and the byte-visible serial sits there in every
        // real capture checked. Hardware-confirmed (2026-09-20, CISC):
        // whatever consumes this specific field reads it as a 28-bit binary
        // value, not free text -- an unconstrained value made DST's "Display
        // Non-Configured Units" screen show a masked/invalid serial. Force
        // the leading character to '0' right here, at this SPD write only --
        // NOT inside as400_get_serial_8() itself, which also backs LOG SENSE
        // page 0x31 for every AS/400 FIXED disk regardless of override;
        // forcing it there broke real PPC load-source recognition (SRC
        // B1014504) the first time this was tried, since page 0x31 does not
        // share this constraint.
        case ZPDB_SERVE_SPD:
            if (len >= 44)
            {
                injectSerial(buf, 36, scsiId);
                buf[36] = '0';
            }
            else if (len > 0 && warnOnce(scsiId, PATCH_WARNED_SPD))
            {
                logmsg("---- WARNING: profile SPD for SCSI ID ", (int)scsiId, " is only ",
                       (int)len, " bytes -- too short to patch, serial override not applied to SPD");
            }
            break;

        // VPD80 (Unit Serial Number): the real captured field width varies
        // (8 or 10 ASCII characters observed so far, always right-justified,
        // space/zero-padded on the left), but the actual per-drive-varying
        // digits are always the LAST 8 bytes of the page -- confirmed across
        // every VPD80 capture in the definitions file regardless of its
        // declared length (20 or 24 bytes seen so far). Requires at least 12
        // bytes total (4-byte page header + >=8 payload) so the write can
        // never reach into the header itself.
        case 0x80:
            if (len >= 12)
            {
                injectSerial(buf, (int)(len - 8), scsiId);
            }
            else if (warnOnce(scsiId, PATCH_WARNED_VPD80))
            {
                logmsg("---- WARNING: profile VPD80 for SCSI ID ", (int)scsiId, " is only ",
                       (int)len, " bytes -- too short to patch, serial override not applied to this page");
            }
            break;

        // VPD82: a fixed-format IBM page, always exactly 48 bytes of payload
        // (52 with the page header) in every real capture seen -- ASCII copy
        // of the serial at offset 14, an EBCDIC copy at offset 38. Gate
        // strictly on the expected length so an unexpected future capture
        // with a differently-shaped VPD82 gets skipped, not silently
        // corrupted.
        case 0x82:
            if (len == 52)
            {
                injectSerial(buf, 14, scsiId);
                injectSerial(buf, 38, scsiId, true);
            }
            else if (warnOnce(scsiId, PATCH_WARNED_VPD82))
            {
                logmsg("---- WARNING: profile VPD82 for SCSI ID ", (int)scsiId, " is ",
                       (int)len, " bytes, not the expected 52 -- serial override not applied to this page");
            }
            break;

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
        // understand IBM's opaque binary encoding at all. The per-model /
        // revision 2-character prefix seen before the ASCII serial
        // (`68`/`F8`/etc.) is preserved automatically either way, since it's
        // part of the untouched, already-captured bytes ahead of the
        // injection point. Injection offset for the ASCII case is simply the
        // descriptor's own declared length byte (buf[7]): descriptor data
        // starts at a fixed buffer offset 8 (4-byte page header + 4-byte
        // descriptor header, both fixed by the SCSI spec), and the serial is
        // the descriptor's own last 8 bytes, so offset = 8 + desc_len - 8 ==
        // desc_len.
        case 0x83:
            if (len >= 16)
            {
                uint8_t codeset = buf[4] & 0x0F;
                uint8_t desigType = buf[5] & 0x0F;
                uint32_t descLen = buf[7];
                if (codeset == 0x02 && desigType == 0x01 && descLen >= 8 &&
                    (8 + descLen) <= len)
                {
                    injectSerial(buf, (int)descLen, scsiId);
                }
                else if (codeset == 0x01 && (desigType == 0x02 || desigType == 0x03) &&
                         descLen >= 1 && (8 + descLen) <= len)
                {
                    buf[8 + descLen - 1] ^= (uint8_t)(scsiId & S2S_CFG_TARGET_ID_BITS);
                }
                else if (warnOnce(scsiId, PATCH_WARNED_VPD83))
                {
                    logmsg("---- profile VPD83 for SCSI ID ", (int)scsiId, " is not a recognized "
                           "T10-vendor-ID/EUI-64/NAA designator shape (codeset=", (int)codeset,
                           " type=", (int)desigType, ") -- left untouched");
                }
            }
            break;

        default:
            break;
    }
}

// Patch the serial and FRU into a page copied out of the built-in AS/400 disk
// identity. These offsets are that one capture's own, not a general property
// of the page shapes -- see applyProfileIdentityFields() above for the path
// that has to cope with any capture.
//
// Unlike the profile path, this runs whether or not an override is
// configured: the built-in identity is a single physical drive's, shared by
// every SCSI ID that falls back to it, so the generated per-ID serial (see
// as400_get_serial_8()) is what keeps those IDs distinct.
static void applyBuiltinIdentityFields(int pageCode, uint8_t *buf, uint32_t len, uint8_t scsiId)
{
    switch (pageCode)
    {
        case ZPDB_SERVE_SPD:
            if (len >= 46)
            {
                // Same 28-bit-value SPD constraint as
                // applyProfileIdentityFields()'s named-profile path above --
                // force the leading character here at the SPD write only, not
                // inside as400_get_serial_8() itself. See that function's own
                // comment for why: it also backs LOG SENSE page 0x31, which
                // does not share this constraint and broke on real PPC
                // hardware when this was forced there instead.
                injectSerial(buf, 38, scsiId);
                buf[38] = '0';
            }
            // The SPD carries only an ASCII copy of the 7-char IBM disk part
            // number at offsets 114-120 -- there is no EBCDIC slot here,
            // unlike VPD page 0x01.
            if (len >= 121) injectPartNumber(buf, 114, -1, scsiId);
            break;

        // ASCII slot at offset 5 and EBCDIC slot at offset 29, 7 bytes each.
        case 0x01: if (len >= 36) injectPartNumber(buf, 5, 29, scsiId); break;

        case 0x80: if (len >= 20) injectSerial(buf, 12, scsiId); break;

        case 0x82:
            // Offset 14, not 16 -- landmark-verified (search for the "IBM"
            // string terminator, read the 8 bytes before it) across 7
            // independently captured real drives (59H7001, 59H6611,
            // 9V8006-041, 86G9124, 55F9806, 45G9463, 45G9463-1), spanning
            // multiple product families. The shipped offset of 16 was off by
            // 2 relative to every real drive checked.
            if (len >= 24) injectSerial(buf, 14, scsiId);
            // VPD82's vendor-specific area also carries an EBCDIC copy of the
            // same serial (SCSI-2 8.3.4.1 table 103 defines the structure,
            // not IBM's use of it) -- offset 38, same as
            // applyProfileIdentityFields()'s named-profile path above,
            // verified against every VPD82 capture in
            // as400_disk_definitions.txt. This built-in identity is shared
            // across every SCSI ID that falls back to it, so its own VPD82
            // needs the same ASCII+EBCDIC treatment to stay internally
            // consistent.
            if (len >= 46) injectSerial(buf, 38, scsiId, true);
            break;

        case 0x83: if (len >= 42) injectSerial(buf, 34, scsiId); break;
        case 0xD1: if (len >= 78) injectSerial(buf, 70, scsiId); break;
        default: break;
    }
}
#endif // PLATFORM_AS400

// Wrapper so the profile-serving paths below stay free of #ifdefs: on a
// non-AS/400 build the flash store can still serve pages, there is simply no
// identity to inject into them.
static void applyProfileIdentity(int pageCode, uint8_t *buf, uint32_t len, uint8_t scsiId)
{
#ifdef PLATFORM_AS400
    applyProfileIdentityFields(pageCode, buf, len, scsiId);
#else
    (void)pageCode;
    (void)buf;
    (void)len;
    (void)scsiId;
#endif
}

bool getCustomVPD(uint8_t scsiId, uint8_t pageCode, uint8_t *buf, uint8_t *length)
{
    uint32_t zlen = 0;
    if (zpdbReadVPD(scsiId, pageCode, buf, MAX_VPD_DATA_SIZE, &zlen) && zlen > 0)
    {
        applyProfileIdentity(pageCode, buf, zlen, scsiId);
        *length = (uint8_t)zlen;
        return true;
    }

#ifdef PLATFORM_AS400
    uint8_t id = scsiId & S2S_CFG_TARGET_ID_BITS;
    const uint8_t *page = nullptr;
    uint8_t len = 0;

    if (g_builtin[id] == BUILTIN_AS400_DISK)
    {
        page = findBuiltinVPD(AS400VitalPages, AS400VitalPagesLen, pageCode, &len);
    }
    else if (g_builtin[id] == BUILTIN_AS400_TAPE_PPC)
    {
        page = findBuiltinVPD(AS400TapePPCVitalPages, AS400TapePPCVitalPagesLen,
                              pageCode, &len);
    }

    if (page != nullptr)
    {
        memcpy(buf, page, len);

        // Tape pages carry no per-drive serial or FRU, matching what the tape
        // loader did when it filled the old table.
        if (g_builtin[id] == BUILTIN_AS400_DISK)
            applyBuiltinIdentityFields(pageCode, buf, len, scsiId);

        *length = len;
        return true;
    }
#endif

    return false;
}

bool getCustomSPD(uint8_t scsiId, uint8_t *buf, uint16_t *length)
{
    uint32_t zlen = 0;
    if (zpdbReadSPD(scsiId, buf, ZPDB_SERVE_MAX, &zlen) && zlen > 0)
    {
        applyProfileIdentity(ZPDB_SERVE_SPD, buf, zlen, scsiId);
        *length = (uint16_t)zlen;
        return true;
    }

#ifdef PLATFORM_AS400
    uint8_t id = scsiId & S2S_CFG_TARGET_ID_BITS;
    const uint8_t *inquiry = nullptr;
    size_t len = 0;

    switch (g_builtin[id])
    {
        case BUILTIN_AS400_DISK:
            inquiry = AS400VendorInquiry;
            len = AS400VendorInquiryLen;
            break;

        case BUILTIN_AS400_TAPE_CISC:
            inquiry = AS400TapeCISCVendorInquiry;
            len = AS400TapeCISCVendorInquiryLen;
            break;

        case BUILTIN_AS400_TAPE_PPC:
            inquiry = AS400TapePPCVendorInquiry;
            len = AS400TapePPCVendorInquiryLen;
            break;

        default:
            break;
    }

    if (inquiry != nullptr && len > 0)
    {
        if (len > ZPDB_SERVE_MAX) len = ZPDB_SERVE_MAX;
        memcpy(buf, inquiry, len);

        if (g_builtin[id] == BUILTIN_AS400_DISK)
        {
            // Vendor (bytes 8-15) and product ID (bytes 16-31) come from the
            // device settings, so a [SCSI<X>] Vendor/Product override takes
            // effect even on the built-in AS/400 identity.
            //
            // Index with the masked `id`, never the raw scsiId: this function
            // is called from scsiInquiry() (inquiry.c) with cfg->scsiId, which
            // carries S2S_CFG_TARGET_ENABLED (0x80) on every enabled target --
            // so ID 5 arrives here as 0x85. getDevice() does no bounds check and
            // m_dev[] only has S2S_MAX_TARGETS+1 entries, so getDevice(0x85) read
            // roughly 12 kB past the end of g_scsi_settings and spliced whatever
            // BSS followed it into the vendor/product fields of every standard
            // INQUIRY -- confirmed against a real AS/400 that rejected the drive
            // while reads and writes kept working. Not caught before because this
            // patch used to run at load time, from parseCustomInquiryData(), which
            // is only ever called with a bare 0..n target index.
            const scsi_device_settings_t *devCfg = g_scsi_settings.getDevice(id);
            if (len >= 16)
                memcpy(buf + 8, devCfg->vendor, sizeof(devCfg->vendor));
            if (len >= 32)
                memcpy(buf + 16, devCfg->prodId, sizeof(devCfg->prodId));

            applyBuiltinIdentityFields(ZPDB_SERVE_SPD, buf, len, scsiId);
        }

        *length = (uint16_t)len;
        return true;
    }
#endif

    return false;
}

bool getCustomModeSense(uint8_t scsiId, uint8_t *buf, uint16_t *length)
{
    uint32_t zlen = 0;
    if (zpdbReadModeSense(scsiId, buf, ZPDB_SERVE_MAX, &zlen) && zlen > 0)
    {
        *length = (uint16_t)zlen;
        return true;
    }

#ifdef PLATFORM_AS400
    uint8_t id = scsiId & S2S_CFG_TARGET_ID_BITS;
    const uint8_t *modeSense = nullptr;
    size_t len = 0;

    if (g_builtin[id] == BUILTIN_AS400_TAPE_CISC)
    {
        modeSense = as400_tape_cisc_mode_sense_all_pages;
        len = as400_tape_cisc_mode_sense_all_pagesLen;
    }
    else if (g_builtin[id] == BUILTIN_AS400_TAPE_PPC)
    {
        modeSense = as400_tape_ppc_mode_sense_all_pages;
        len = as400_tape_ppc_mode_sense_all_pagesLen;
    }

    if (modeSense != nullptr && len > 0)
    {
        if (len > ZPDB_SERVE_MAX) len = ZPDB_SERVE_MAX;
        memcpy(buf, modeSense, len);

        // MODE SENSE(6) header offset 1 is Medium Type. The compiled-in
        // capture was taken with no cartridge loaded (0x00, "no
        // cartridge") -- real hardware only reports 0x00 when genuinely
        // empty (see AS400TapeCISCMediumType/AS400TapePPCMediumType's own
        // comments), so serving it as-is once a tape image is actually
        // configured makes every density request fail identically,
        // regardless of which density is asked for. An explicit
        // MediumType= override in this ID's ini section (the same generic
        // per-device key doModeSense()'s non-AS/400 fallback path honors)
        // takes precedence over the compiled-in default.
        if (len >= 2)
        {
            const scsi_device_settings_t *devCfg = g_scsi_settings.getDevice(id);
            if (devCfg->mediumType >= 0)
            {
                buf[1] = (uint8_t)devCfg->mediumType;
            }
            else if (g_builtin[id] == BUILTIN_AS400_TAPE_CISC)
            {
                buf[1] = AS400TapeCISCMediumType;
            }
            else
            {
                buf[1] = AS400TapePPCMediumType;
            }
        }

        *length = (uint16_t)len;
        return true;
    }
#endif

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

bool isAS400CapturedTapeIdentity(uint8_t scsiId)
{
    uint8_t id = scsiId & S2S_CFG_TARGET_ID_BITS;
    return g_builtin[id] == BUILTIN_AS400_TAPE_CISC || g_builtin[id] == BUILTIN_AS400_TAPE_PPC;
}
#endif
