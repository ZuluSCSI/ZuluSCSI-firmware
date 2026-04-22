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
#endif

#include <scsi.h>
#include <minIni.h>
#include <string.h>
#include <stdlib.h>

// Storage for custom VPD pages: up to 16 entries across all SCSI IDs
// Each entry: [0]=scsiId, [1]=pageCode, [2]=length, [3..]=data
#define MAX_CUSTOM_VPD_ENTRIES 16
#define MAX_VPD_DATA_SIZE 128
static struct {
    uint8_t scsiId;
    uint8_t pageCode;
    uint8_t length;
    uint8_t data[MAX_VPD_DATA_SIZE];
} g_custom_vpd[MAX_CUSTOM_VPD_ENTRIES];
static int g_custom_vpd_count = 0;

// Storage for custom standard inquiry data per SCSI ID
#define MAX_SPD_SIZE 128
static struct {
    uint8_t length;
    uint8_t data[MAX_SPD_SIZE];
} g_custom_spd[S2S_MAX_TARGETS];

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

#ifdef PLATFORM_AS400
// Inject the generated serial number into a VPD page at the given offset
static void injectSerial(uint8_t *data, int offset, uint8_t scsiId)
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

    memcpy(data + offset, serial, 8);
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
#endif

#ifdef PLATFORM_AS400
// Populate default AS/400 inquiry and VPD data
// Only fills in data that wasn't already provided via INI.
static void loadAS400Defaults(uint8_t scsiId,S2S_CFG_TYPE type)
{
    bool loaded_default_data = false;

    const S2S_TargetCfg *config = scsiDev.targets[scsiId].cfg;
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
        if (len >= 46)
            injectSerial(g_custom_spd[scsiId].data, 38, scsiId);
        if (len >= 121)
            injectPartNumber(g_custom_spd[scsiId].data, 114, -1, scsiId);
        g_custom_spd[scsiId].length = len;
        loaded_default_data = true;
    }

    // Default VPD pages
    for (size_t p = 0; p < AS400VitalPagesLen && g_custom_vpd_count < MAX_CUSTOM_VPD_ENTRIES; p++)
    {
        uint8_t pageLen = AS400VitalPages[p][0]; // first byte is length
        if (pageLen < 2) continue;
        uint8_t pageCode = AS400VitalPages[p][2]; // page code at offset 2 in data

        if (hasCustomVPD(scsiId, pageCode))
            continue; // INI override takes precedence

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
            injectSerial(g_custom_vpd[idx].data, 16, scsiId);
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
    if (loaded_default_data)
    {
        logmsg("---- Loaded default AS/400 inquiry data for SCSI ID ", (int) scsiId);
    }
}
#endif

void parseCustomInquiryData(uint8_t scsiId, S2S_CFG_TYPE type)
{
    char tmp[512];
    char section[6] = "SCSI0";
    char key[8];

    g_custom_vpd_count = 0;
    memset(g_custom_spd, 0, sizeof(g_custom_spd));
#ifdef PLATFORM_AS400
    memset(g_as400_serial_override, 0, sizeof(g_as400_serial_override));
    memset(g_as400_part_override, 0, sizeof(g_as400_part_override));
#endif


    section[4] = scsiEncodeID(scsiId);

    // Parse VPD pages: vpd00, vpd80, etc.
    for (int page = 0; page < 0xFF && g_custom_vpd_count < MAX_CUSTOM_VPD_ENTRIES; page++)
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

    // Load AS/400 defaults for any IDs that don't have INI overrides
    loadAS400Defaults(scsiId, type);
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

