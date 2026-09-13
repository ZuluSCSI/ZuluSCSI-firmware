/**
 * ZuluSCSI™ - Copyright (c) 2022-2025 Rabbit Hole Computing™
 *
 * This file is licensed under the GPL version 3 or any later version.
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

// See ZuluSCSI_partition_table.h for why this duplicates part of the
// vendored SdFat library instead of extending it in place.

#include "ZuluSCSI_partition_table.h"
#include "ZuluSCSI.h"
#include "ZuluSCSI_log.h"
#include <string.h>

#define SD_SECTOR_SIZE 512

// ---- little-endian field readers (on-disk fields are byte arrays, not
// native ints, so this works regardless of the MCU's own endianness even
// though RP2040/RP2350 are little-endian anyway) ----

static uint32_t getLe32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t getLe64(const uint8_t *p)
{
    return (uint64_t)getLe32(p) | ((uint64_t)getLe32(p + 4) << 32);
}

// ---- CRC-32/ISO-HDLC (the "zlib" polynomial, 0xEDB88320 reflected) --
// this is what the UEFI/GPT spec mandates for both the header and
// partition-entry-array checksums. No table: this runs at most a few
// times per boot (only when a PART:n image is actually opened), not a
// hot path, so the code-size/simplicity tradeoff favors the bit-loop form
// over a 256-entry lookup table.
//
// Operates on the RAW (non-complemented) running state so it can be fed
// data across multiple calls (e.g. a partition-entry array spanning
// several sectors) -- wrap a sequence of calls in CRC32_INIT/CRC32_FINISH,
// never complement in between.
#define CRC32_INIT 0xFFFFFFFFu
#define CRC32_FINISH(crc) (~(crc))

static uint32_t crc32_raw_update(uint32_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++)
        {
            uint32_t mask = -(crc & 1);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc;
}

// ---- MBR ----
// Fixed layout, no integrity field exists in the format itself.
#define MBR_PART_TABLE_OFFSET 446
#define MBR_PART_ENTRY_SIZE 16
#define MBR_SIGNATURE_OFFSET 510
#define MBR_SIGNATURE 0xAA55

static bool mbrResolve(uint32_t partitionNumber, partition_extent_t *out)
{
    if (partitionNumber < 1 || partitionNumber > 4)
        return false; // extended/logical MBR partitions: out of scope, see header comment

    uint8_t sector[SD_SECTOR_SIZE];
    if (!SD.card()->readSector(0, sector))
    {
        logmsg("---- PART: failed to read MBR sector 0 from SD card");
        return false;
    }

    if (sector[MBR_SIGNATURE_OFFSET] != (MBR_SIGNATURE & 0xFF) ||
        sector[MBR_SIGNATURE_OFFSET + 1] != (MBR_SIGNATURE >> 8))
    {
        logmsg("---- PART: MBR signature 0x55AA not found at sector 0 offset 510 -- no valid partition table");
        return false;
    }

    const uint8_t *entry = sector + MBR_PART_TABLE_OFFSET + (partitionNumber - 1) * MBR_PART_ENTRY_SIZE;
    uint32_t startSector = getLe32(entry + 8);
    uint32_t sectorCount = getLe32(entry + 12);

    if (entry[4] == 0 || sectorCount == 0)
        return false; // empty slot (type 0) or zero-length entry: partition doesn't exist

    out->startSector = startSector;
    out->sectorCount = sectorCount;
    out->isGPT = false;
    return true;
}

// ---- GPT ----
// "EFI PART", as two little-endian uint32 reads (bytes 0-3, then 4-7).
#define GPT_SIGNATURE_WORD0 0x20494645ul // "EFI "
#define GPT_SIGNATURE_WORD1 0x54524150ul // "PART"
#define GPT_HEADER_LBA 1

// Byte offsets within the 92-byte GPT header (see UEFI spec 5.3.2).
#define GPT_HDR_SIGNATURE 0
#define GPT_HDR_HEADER_SIZE 12
#define GPT_HDR_CRC32 16
#define GPT_HDR_BACKUP_LBA 32
#define GPT_HDR_PART_ENTRY_START_LBA 72
#define GPT_HDR_NUM_PART_ENTRIES 80
#define GPT_HDR_PART_ENTRY_SIZE 84
#define GPT_HDR_CRC32_PART_ARRAY 88

typedef struct
{
    // backup_lba is deliberately not surfaced here -- see gptResolve()'s
    // comment on why the backup location is computed independently
    // (SD.card()->sectorCount()-1) rather than trusted from a header that
    // may have already failed its own CRC check.
    uint64_t partEntryStartLba;
    uint32_t numPartEntries;
    uint32_t partEntrySize;
} gpt_header_info_t;

// Validates the signature and header CRC32 of the GPT header at `headerLba`.
// On success, fills in the fields needed to locate/size the partition
// entry array. Returns false (with a log message identifying which check
// failed) if anything doesn't check out -- never trusts a header whose
// own CRC didn't validate.
static bool gptReadHeader(uint32_t headerLba, gpt_header_info_t *info, bool quietIfAbsent)
{
    uint8_t sector[SD_SECTOR_SIZE];
    if (!SD.card()->readSector(headerLba, sector))
    {
        logmsg("---- PART: failed to read GPT header at LBA ", (int)headerLba);
        return false;
    }

    if (getLe32(sector + GPT_HDR_SIGNATURE) != GPT_SIGNATURE_WORD0 ||
        getLe32(sector + GPT_HDR_SIGNATURE + 4) != GPT_SIGNATURE_WORD1)
    {
        // Absent signature just means "not a GPT disk" for the primary
        // location -- a completely normal, expected case for MBR cards,
        // not corruption. Only log when checking a location that should
        // have a valid header (i.e. the backup, once we already know
        // this is supposed to be a GPT disk).
        if (!quietIfAbsent)
            logmsg("---- PART: GPT signature \"EFI PART\" not found at LBA ", (int)headerLba);
        return false;
    }

    uint32_t headerSize = getLe32(sector + GPT_HDR_HEADER_SIZE);
    if (headerSize < 92 || headerSize > SD_SECTOR_SIZE)
    {
        logmsg("---- PART: GPT header at LBA ", (int)headerLba, " has implausible header_size ", (int)headerSize, ", treating as corrupt");
        return false;
    }

    uint32_t storedCrc = getLe32(sector + GPT_HDR_CRC32);

    // Per UEFI spec: header CRC is computed over header_size bytes with
    // the crc32 field itself treated as zero during the calculation.
    uint8_t headerCopy[SD_SECTOR_SIZE];
    memcpy(headerCopy, sector, sizeof(headerCopy));
    memset(headerCopy + GPT_HDR_CRC32, 0, 4);
    uint32_t computedCrc = CRC32_FINISH(crc32_raw_update(CRC32_INIT, headerCopy, headerSize));

    if (computedCrc != storedCrc)
    {
        logmsg("---- PART: GPT header CRC32 mismatch at LBA ", (int)headerLba,
               " (stored ", (int)storedCrc, ", computed ", (int)computedCrc, ") -- partition table is corrupt");
        return false;
    }

    info->partEntryStartLba = getLe64(sector + GPT_HDR_PART_ENTRY_START_LBA);
    info->numPartEntries = getLe32(sector + GPT_HDR_NUM_PART_ENTRIES);
    info->partEntrySize = getLe32(sector + GPT_HDR_PART_ENTRY_SIZE);

    if (info->partEntrySize < 128 || (info->partEntrySize % 8) != 0 ||
        info->numPartEntries == 0 || info->numPartEntries > 1024)
    {
        logmsg("---- PART: GPT header at LBA ", (int)headerLba, " has implausible partition-entry-array"
               " geometry (", (int)info->numPartEntries, " entries of ", (int)info->partEntrySize,
               " bytes), treating as corrupt");
        return false;
    }

    uint32_t storedArrayCrc = getLe32(sector + GPT_HDR_CRC32_PART_ARRAY);

    // Validate the partition-entry array's own CRC32, reading it sector
    // by sector rather than allocating num_part_entries*part_entry_size
    // bytes at once (that array is spec-sized for up to 128 entries of
    // 128 bytes = 16KB by default, too much to put on the stack here).
    uint32_t arrayBytes = info->numPartEntries * info->partEntrySize;
    uint32_t crc = CRC32_INIT;
    uint32_t remaining = arrayBytes;
    uint32_t lba = (uint32_t)info->partEntryStartLba;
    while (remaining > 0)
    {
        uint8_t buf[SD_SECTOR_SIZE];
        if (!SD.card()->readSector(lba, buf))
        {
            logmsg("---- PART: failed to read GPT partition entry array at LBA ", (int)lba);
            return false;
        }
        uint32_t chunk = remaining < SD_SECTOR_SIZE ? remaining : SD_SECTOR_SIZE;
        crc = crc32_raw_update(crc, buf, chunk);
        remaining -= chunk;
        lba++;
    }
    uint32_t arrayCrc = CRC32_FINISH(crc);

    if (arrayCrc != storedArrayCrc)
    {
        logmsg("---- PART: GPT partition-entry-array CRC32 mismatch (table referenced from LBA ",
               (int)headerLba, ", entries at LBA ", (int)info->partEntryStartLba,
               ", stored ", (int)storedArrayCrc, ", computed ", (int)arrayCrc,
               ") -- partition table is corrupt");
        return false;
    }

    return true;
}

static bool gptResolve(uint32_t partitionNumber, partition_extent_t *out)
{
    gpt_header_info_t info;
    bool usingBackup = false;

    if (!gptReadHeader(GPT_HEADER_LBA, &info, /*quietIfAbsent=*/true))
    {
        // Distinguish "no GPT at all" (silent, fall through to MBR by the
        // caller) from "GPT present but corrupt" (already logged loudly
        // above by gptReadHeader) by re-checking just the signature here.
        uint8_t sector[SD_SECTOR_SIZE];
        bool signaturePresent = SD.card()->readSector(GPT_HEADER_LBA, sector) &&
                                 getLe32(sector + GPT_HDR_SIGNATURE) == GPT_SIGNATURE_WORD0 &&
                                 getLe32(sector + GPT_HDR_SIGNATURE + 4) == GPT_SIGNATURE_WORD1;
        if (!signaturePresent)
            return false; // no GPT on this card -- normal, not an error

        // Primary signature present but failed validation: try the
        // backup copy at the last sector of the disk. Deliberately does
        // NOT trust the primary's own backup_lba field for this -- a
        // header that already failed its own CRC can't be trusted for
        // anything, including where it claims its backup lives. The GPT
        // spec places the backup at the disk's last sector, which we can
        // compute independently.
        uint32_t totalSectors = SD.card()->sectorCount();
        if (totalSectors == 0)
        {
            logmsg("---- PART: cannot determine SD card size to locate GPT backup header, giving up");
            return false;
        }

        logmsg("---- PART: retrying with backup GPT header at LBA ", (int)(totalSectors - 1));
        if (!gptReadHeader(totalSectors - 1, &info, /*quietIfAbsent=*/false))
        {
            logmsg("---- PART: GPT partition table corrupt in both primary and backup copies, giving up");
            return false;
        }

        usingBackup = true;
    }

    if (usingBackup)
        logmsg("---- PART: recovered GPT partition table from backup copy");

    if (partitionNumber < 1 || partitionNumber > info.numPartEntries || partitionNumber > PARTITION_TABLE_MAX_INDEX)
        return false;

    uint32_t entryLba = (uint32_t)info.partEntryStartLba + ((partitionNumber - 1) * info.partEntrySize) / SD_SECTOR_SIZE;
    uint32_t entryOffsetInSector = ((partitionNumber - 1) * info.partEntrySize) % SD_SECTOR_SIZE;

    uint8_t sector[SD_SECTOR_SIZE];
    if (!SD.card()->readSector(entryLba, sector))
    {
        logmsg("---- PART: failed to read GPT partition entry for partition ", (int)partitionNumber);
        return false;
    }

    const uint8_t *entry = sector + entryOffsetInSector;
    static const uint8_t zeroGuid[16] = {0};
    if (memcmp(entry, zeroGuid, 16) == 0)
        return false; // unused entry slot: partition doesn't exist

    uint64_t firstLba = getLe64(entry + 32);
    uint64_t lastLba = getLe64(entry + 40);

    if (lastLba < firstLba || firstLba > 0xFFFFFFFFull || lastLba > 0xFFFFFFFFull)
    {
        logmsg("---- PART: GPT partition ", (int)partitionNumber, " extent (", (unsigned long long)firstLba,
               "-", (unsigned long long)lastLba, ") is invalid or exceeds the 32-bit sector range this"
               " firmware supports elsewhere");
        return false;
    }

    out->startSector = (uint32_t)firstLba;
    out->sectorCount = (uint32_t)(lastLba - firstLba + 1);
    out->isGPT = true;
    return true;
}

bool partitionTableResolve(uint32_t partitionNumber, partition_extent_t *out)
{
    // GPT first: it has a clearly identifiable signature, and it's common
    // for GPT-partitioned drives to carry a protective MBR too (which
    // would otherwise be misread as a single, whole-disk MBR partition).
    if (gptResolve(partitionNumber, out))
        return true;

    return mbrResolve(partitionNumber, out);
}
