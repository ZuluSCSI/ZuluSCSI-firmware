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

#include "ZuluSCSI_sd_fsfree.h"
#include "ZuluSCSI_platform.h"
#include "ZuluSCSI.h"
#include "ZuluSCSI_disk.h"
#include <ZuluSCSI_log.h>
#include <string.h>

extern bool g_rawdrive_active;

// How the allocation state is stored, which the file system library keeps to
// itself: exFAT has an allocation bitmap that starts at the first cluster of
// the cluster area (the library does not support any other place), one bit per
// cluster, set when allocated. FAT32 has one 32 bit entry per cluster in the
// FAT, zero when free.

#define FS_FAT32 32
#define FS_EXFAT 64

static struct
{
    bool usable;
    uint8_t type;
    uint32_t heap_start;     // first sector of the cluster area (cluster 2)
    uint32_t cluster_count;  // clusters in the cluster area
    uint32_t sectors_per_cluster;
    uint32_t fat_start;      // FAT32 only
    uint32_t cached_sector;  // sector in scsiDev.data, if cached_valid
    bool cached_valid;
    bool read_failed;
} g;

void sdFsFreeReset()
{
    memset(&g, 0, sizeof(g));
}

// True if some target lets the host write to the card without going through
// the file system library.
static bool card_exposed_to_host()
{
    if (g_rawdrive_active)
    {
        return true;
    }

    for (int i = 0; i < S2S_MAX_TARGETS; i++)
    {
        image_config_t &img = scsiDiskGetImageConfig(i);
        if (img.file.isOpen() && img.file.isRaw())
        {
            return true;
        }
    }
    return false;
}

// Writes out everything the file system library still holds in its caches, so
// the card has the current allocation state.
static bool flush_file_system()
{
    FsFile root = SD.open("/", O_RDONLY);
    if (!root)
    {
        return false;
    }
    bool ok = root.sync();
    root.close();
    return ok;
}

// Sector in scsiDev.data, read once.
static const uint8_t *get_sector(uint32_t sector)
{
    if (!g.cached_valid || g.cached_sector != sector)
    {
        g.cached_valid = false;
        if (!SD.card()->readSector(sector, scsiDev.data))
        {
            g.read_failed = true;
            return nullptr;
        }
        g.cached_sector = sector;
        g.cached_valid = true;
    }
    return scsiDev.data;
}

// Cluster number 2 is index 0.
static bool cluster_is_free(uint32_t index)
{
    if (g.type == FS_EXFAT)
    {
        uint32_t byte = index >> 3;
        const uint8_t *sector = get_sector(g.heap_start + byte / 512);
        return sector && !(sector[byte % 512] & (1 << (index & 7)));
    }
    else
    {
        uint32_t offset = (index + 2) * 4;
        const uint8_t *sector = get_sector(g.fat_start + offset / 512);
        if (!sector)
        {
            return false;
        }
        const uint8_t *e = sector + offset % 512;
        uint32_t value = e[0] | (e[1] << 8) | (e[2] << 16) | ((uint32_t)e[3] << 24);
        return (value & 0x0FFFFFFF) == 0;
    }
}

// The clusters that hold SD sectors [first, first + count) are allocated.
static bool sectors_are_allocated(uint32_t first, uint32_t count)
{
    uint32_t span = g.cluster_count * g.sectors_per_cluster;
    if (count == 0 || first < g.heap_start || first - g.heap_start >= span ||
        count > span - (first - g.heap_start))
    {
        return false;
    }
    uint32_t c0 = (first - g.heap_start) / g.sectors_per_cluster;
    uint32_t c1 = (first - g.heap_start + count - 1) / g.sectors_per_cluster;
    return !cluster_is_free(c0) && !cluster_is_free(c1) && !g.read_failed;
}

bool sdFsFreeStart()
{
    sdFsFreeReset();

    FsVolume *vol = SD.vol();
    if (!g_sdcard_present || !vol)
    {
        return false;
    }

    uint8_t type = vol->fatType();
    if (type != FS_EXFAT && type != FS_FAT32)
    {
        logmsg("SD scrubber: file system free space is not used, FAT", (int)type, " is not supported");
        return false;
    }

    if (card_exposed_to_host())
    {
        logmsg("SD scrubber: file system free space is not used, a target gives the host direct access to the card");
        return false;
    }

    g.type = type;
    g.heap_start = vol->dataStartSector();
    g.cluster_count = vol->clusterCount();
    g.sectors_per_cluster = vol->sectorsPerCluster();
    g.fat_start = vol->fatStartSector();
    if (g.cluster_count == 0 || g.sectors_per_cluster == 0)
    {
        return false;
    }

    if (!flush_file_system())
    {
        logmsg("SD scrubber: file system free space is not used, the file system could not be written out");
        return false;
    }

    // Is the allocation state read the way the file system library reads it?
    // A mistake here would release clusters that hold files, so check twice.
    //
    // 1. exFAT: the number of free clusters counted here must be the library's.
    if (type == FS_EXFAT)
    {
        uint32_t used = 0;
        uint32_t bytes = (g.cluster_count + 7) / 8;
        for (uint32_t offset = 0; offset < bytes; offset += 512)
        {
            const uint8_t *sector = get_sector(g.heap_start + offset / 512);
            if (!sector)
            {
                return false;
            }
            uint32_t n = bytes - offset < 512 ? bytes - offset : 512;
            for (uint32_t i = 0; i < n; i++)
            {
                uint8_t b = sector[i];
                if (offset + i == bytes - 1 && (g.cluster_count & 7))
                {
                    b &= (1 << (g.cluster_count & 7)) - 1; // padding bits
                }
                while (b)
                {
                    used += b & 1;
                    b >>= 1;
                }
            }
            platform_reset_watchdog();
        }

        int32_t library_free = vol->freeClusterCount();
        if (library_free < 0 || (uint32_t)library_free != g.cluster_count - used)
        {
            logmsg("SD scrubber: file system free space is not used, the allocation bitmap does not match the file system library (",
                   (int)(g.cluster_count - used), " free clusters against ", (int)library_free, ")");
            sdFsFreeReset();
            return false;
        }
    }

    // 2. Both types: the clusters that the file system library placed the
    // image files in must read as allocated, at the start, middle and end.
    // At least one image has to prove this.
    uint32_t proven = 0;
    for (int i = 0; i < S2S_MAX_TARGETS; i++)
    {
        image_config_t &img = scsiDiskGetImageConfig(i);
        uint32_t bgn = 0;
        uint8_t mode = 0;
        if (!img.file.isOpen() || !img.file.linearSdMapping(&bgn, &mode))
        {
            continue;
        }

        uint32_t sectors = (uint32_t)((img.file.size() + 511) / 512);
        if (sectors == 0)
        {
            continue;
        }
        if (!sectors_are_allocated(bgn, 1) ||
            !sectors_are_allocated(bgn + sectors / 2, 1) ||
            !sectors_are_allocated(bgn + sectors - 1, 1))
        {
            logmsg("SD scrubber: file system free space is not used, the free space bookkeeping on the card does not match where image ",
                   i, " is stored");
            sdFsFreeReset();
            return false;
        }
        proven++;
    }

    if (proven == 0 && type == FS_FAT32)
    {
        logmsg("SD scrubber: file system free space is not used, no image file could be used to check the FAT");
        sdFsFreeReset();
        return false;
    }

    g.cached_valid = false;
    g.read_failed = false;
    g.usable = true;
    logmsg("SD scrubber: file system (FAT", (int)type, ") free space will be released without reading it, ",
           (int)g.cluster_count, " clusters of ", (int)(g.sectors_per_cluster / 2), " KB");
    return true;
}

bool sdFsRangeFree(uint32_t firstSector, uint32_t sectors)
{
    if (!g.usable || sectors == 0 || card_exposed_to_host())
    {
        return false;
    }

    uint32_t span = g.cluster_count * g.sectors_per_cluster;
    if (firstSector < g.heap_start || firstSector - g.heap_start >= span ||
        sectors > span - (firstSector - g.heap_start))
    {
        return false;
    }

    // Anything the file system library has not written out yet, such as a
    // cluster a file has just been given, must be on the card.
    if (!flush_file_system())
    {
        return false;
    }

    g.cached_valid = false;
    g.read_failed = false;

    uint32_t c0 = (firstSector - g.heap_start) / g.sectors_per_cluster;
    uint32_t c1 = (firstSector - g.heap_start + sectors - 1) / g.sectors_per_cluster;
    for (uint32_t c = c0; c <= c1; c++)
    {
        if (!cluster_is_free(c))
        {
            return false;
        }
    }
    return !g.read_failed;
}
