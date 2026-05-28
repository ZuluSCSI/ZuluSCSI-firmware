/*
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

#include "ZuluSCSI_control_api.h"
#include "ZuluSCSI_disk.h"
#include "ZuluSCSI_cdrom.h"
#include "ZuluSCSI_log.h"
#include "ZuluSCSI_config.h"
#include <minIni.h>
#include <SdFat.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

extern "C" {
#include <scsi2sd.h>
}

// SD card global declared in ZuluSCSI.h
extern SdFs SD;

// -----------------------------------------------------------------------
// Internal helpers
// -----------------------------------------------------------------------

static bool isRemovableType(S2S_CFG_TYPE type)
{
    switch (type)
    {
        case S2S_CFG_OPTICAL:
        case S2S_CFG_REMOVABLE:
        case S2S_CFG_FLOPPY_14MB:
        case S2S_CFG_MO:
        case S2S_CFG_SEQUENTIAL:
        case S2S_CFG_ZIP100:
            return true;
        default:
            return false;
    }
}

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------

bool controlIsRemovableDevice(uint8_t scsi_id)
{
    if (scsi_id >= S2S_MAX_TARGETS) return false;
    image_config_t &img = scsiDiskGetImageConfig(scsi_id);
    if (!(img.scsiId & S2S_CFG_TARGET_ENABLED)) return false;
    return isRemovableType((S2S_CFG_TYPE)img.deviceType);
}

int controlListRemovableDevices(uint8_t *ids_out, int max_count)
{
    int count = 0;
    for (uint8_t i = 0; i < S2S_MAX_TARGETS && count < max_count; i++)
    {
        if (controlIsRemovableDevice(i))
            ids_out[count++] = i;
    }
    return count;
}

const char* controlGetDeviceTypeName(uint8_t scsi_id)
{
    if (scsi_id >= S2S_MAX_TARGETS) return "Unknown";
    image_config_t &img = scsiDiskGetImageConfig(scsi_id);
    if (!(img.scsiId & S2S_CFG_TARGET_ENABLED)) return "Inactive";
    switch ((S2S_CFG_TYPE)img.deviceType)
    {
        case S2S_CFG_OPTICAL:     return "CD-ROM";
        case S2S_CFG_REMOVABLE:   return "Removable";
        case S2S_CFG_FLOPPY_14MB: return "Floppy";
        case S2S_CFG_MO:          return "MO";
        case S2S_CFG_SEQUENTIAL:  return "Tape";
        case S2S_CFG_ZIP100:      return "Zip";
        default:                  return "Unknown";
    }
}

bool controlGetCurrentImage(uint8_t scsi_id, char *buf, size_t buflen)
{
    if (!buf || buflen == 0) return false;
    buf[0] = '\0';
    if (scsi_id >= S2S_MAX_TARGETS) return false;

    image_config_t &img = scsiDiskGetImageConfig(scsi_id);
    if (!(img.scsiId & S2S_CFG_TARGET_ENABLED)) return false;
    if (img.ejected || !img.file.isOpen() || img.current_image[0] == '\0')
        return false;

    strncpy(buf, img.current_image, buflen - 1);
    buf[buflen - 1] = '\0';
    return true;
}

bool controlGetImageDirectory(uint8_t scsi_id, char *buf, size_t buflen)
{
    if (!buf || buflen == 0) return false;
    buf[0] = '\0';
    if (scsi_id >= S2S_MAX_TARGETS) return false;

    image_config_t &img = scsiDiskGetImageConfig(scsi_id);
    if (!(img.scsiId & S2S_CFG_TARGET_ENABLED)) return false;

    // Explicit ImgDir in zuluscsi.ini takes first priority
    char section[] = "SCSI0";
    section[4] =  scsiEncodeID(scsi_id);
    char dirname[MAX_FILE_PATH];
    int dirlen = ini_gets(section, "ImgDir", "", dirname, sizeof(dirname), CONFIGFILE);
    if (dirlen > 0)
    {
        strncpy(buf, dirname, buflen - 1);
        buf[buflen - 1] = '\0';
        return true;
    }

    // Derive from the currently loaded image path
    if (img.image_directory && img.current_image[0] != '\0')
    {
        strncpy(buf, img.current_image, buflen - 1);
        buf[buflen - 1] = '\0';
        char *last_slash = strrchr(buf, '/');
        if (last_slash && last_slash != buf)
        {
            *last_slash = '\0';
            return true;
        }
    }

    // Prefix-mode: images sit in the root directory filtered by a 3-char prefix
    if (img.use_prefix)
    {
        if (buflen >= 2) { buf[0] = '/'; buf[1] = '\0'; }
        return true;
    }

    // Fall back to the conventional directory name for this device type
    if (!img.image_directory) return false;

    char prefix[4];
    switch ((S2S_CFG_TYPE)img.deviceType)
    {
        case S2S_CFG_OPTICAL:     strncpy(prefix, "CD0", 4); break;
        case S2S_CFG_REMOVABLE:   strncpy(prefix, "RE0", 4); break;
        case S2S_CFG_SEQUENTIAL:  strncpy(prefix, "TP0", 4); break;
        case S2S_CFG_FLOPPY_14MB: strncpy(prefix, "FD0", 4); break;
        case S2S_CFG_MO:          strncpy(prefix, "MO0", 4); break;
        case S2S_CFG_ZIP100:      strncpy(prefix, "ZP0", 4); break;
        default: return false;
    }
    prefix[2] = scsiEncodeID(scsi_id);
    memcpy(buf, prefix, sizeof(prefix));
    return true;
}

bool controlGetImagePrefix(uint8_t scsi_id, char *buf, size_t buflen)
{
    if (!buf || buflen < 4) return false;
    buf[0] = '\0';
    if (scsi_id >= S2S_MAX_TARGETS) return false;
    image_config_t &img = scsiDiskGetImageConfig(scsi_id);
    if (!(img.scsiId & S2S_CFG_TARGET_ENABLED)) return false;
    if (!img.use_prefix) return false;

    const char *tp;
    switch ((S2S_CFG_TYPE)img.deviceType)
    {
        case S2S_CFG_OPTICAL:     tp = "cd"; break;
        case S2S_CFG_REMOVABLE:   tp = "re"; break;
        case S2S_CFG_SEQUENTIAL:  tp = "tp"; break;
        case S2S_CFG_FLOPPY_14MB: tp = "fd"; break;
        case S2S_CFG_MO:          tp = "mo"; break;
        case S2S_CFG_ZIP100:      tp = "zp"; break;
        default: return false;
    }
    buf[0] = tp[0];
    buf[1] = tp[1];
    buf[2] = scsiEncodeID(scsi_id);
    buf[3] = '\0';
    return true;
}

// Flat FsFile scan for exactly one .cue file — identifies a directory as a
// CUE/BIN disc set without calling WalkDirectory (avoids stack overflow).
// total_size receives the sum of all file sizes in the directory (0 on false).
static bool isDirACueBinSet(const char *dirpath, uint64_t &total_size)
{
    total_size = 0;

    FsFile dir;
    if (!dir.open(dirpath))
        return false;

    char name[MAX_FILE_PATH];
    int cue_count = 0;

    FsFile entry;
    while (entry.openNext(&dir, O_RDONLY))
    {
        if (!entry.isDir() && !entry.isHidden() && entry.getName(name, sizeof(name)))
        {
            total_size += (uint64_t)entry.size();
            const char *ext = strrchr(name, '.');
            if (ext && strcasecmp(ext, ".cue") == 0)
                cue_count++;
        }
        entry.close();
    }

    dir.close();
    if (cue_count != 1)
    {
        total_size = 0;
        return false;
    }
    return true;
}

// controlListImages — single-pass flat FsFile scan, deliberately avoids
// SDNavigator/WalkDirectory (stack-overflow risk on RP2350).
// For root-directory prefix-mode devices the list is filtered to entries
// whose names begin with the 3-char type+ID prefix (e.g. "cd4").
int controlListImages(uint8_t scsi_id,
    void (*callback)(int idx, const char *filename,
                     const char *full_path, uint64_t size, bool is_dir, void *userdata),
    void *userdata)
{
    char imgdir[MAX_FILE_PATH];
    if (!controlGetImageDirectory(scsi_id, imgdir, sizeof(imgdir)))
        return 0;

    // For root "/" avoid producing "//name"
    bool root_dir = (imgdir[0] == '/' && imgdir[1] == '\0');

    char imgprefix[4];
    bool filter_prefix = controlGetImagePrefix(scsi_id, imgprefix, sizeof(imgprefix));

    FsFile dir;
    if (!dir.open(imgdir))
        return 0;

    char name[MAX_FILE_PATH];
    char full_path[MAX_FILE_PATH];
    int count = 0;

    FsFile entry;
    while (entry.openNext(&dir, O_RDONLY))
    {
        if (!entry.getName(name, sizeof(name)) || entry.isHidden())
        {
            entry.close();
            continue;
        }

        bool is_dir = entry.isDir();
        uint64_t size = is_dir ? 0 : (uint64_t)entry.size();
        entry.close();

        if (filter_prefix && strncasecmp(name, imgprefix, 3) != 0)
            continue;

        if (root_dir)
            snprintf(full_path, sizeof(full_path), "/%s", name);
        else
            snprintf(full_path, sizeof(full_path), "%s/%s", imgdir, name);

        if (is_dir)
        {
            if (!isDirACueBinSet(full_path, size))
                continue;
        }
        else if (!scsiDiskFilenameValid(name))
        {
            continue;
        }

        if (callback)
            callback(count, name, full_path, size, is_dir, userdata);
        count++;
    }

    dir.close();
    return count;
}

bool controlLoadImage(uint8_t scsi_id, const char *full_path)
{
    if (scsi_id >= S2S_MAX_TARGETS) return false;
    image_config_t &img = scsiDiskGetImageConfig(scsi_id);
    if (!(img.scsiId & S2S_CFG_TARGET_ENABLED)) return false;
    if (!isRemovableType((S2S_CFG_TYPE)img.deviceType)) return false;

    if (full_path == nullptr || full_path[0] == '\0')
    {
        // Advance to the next image in sequence with a media-change event
        char next_name[MAX_FILE_PATH];
        if (scsiDiskGetNextImageName(img, next_name, sizeof(next_name)) <= 0)
        {
            logmsg("Control: no next image found for SCSI ID ", (int)scsi_id);
            return false;
        }
        logmsg("Control: advancing to next image '", next_name,
               "' on SCSI ID ", (int)scsi_id);
        return switchNextImage(img, next_name);
    }

    logmsg("Control: loading '", full_path, "' on SCSI ID ", (int)scsi_id);
    return switchNextImage(img, full_path);
}

bool controlEjectMedia(uint8_t scsi_id)
{
    if (scsi_id >= S2S_MAX_TARGETS) return false;
    image_config_t &img = scsiDiskGetImageConfig(scsi_id);
    if (!(img.scsiId & S2S_CFG_TARGET_ENABLED)) return false;
    if (!isRemovableType((S2S_CFG_TYPE)img.deviceType)) return false;

    if (img.ejected) return true;

    logmsg("Control: ejecting SCSI ID ", (int)scsi_id);
    if ((S2S_CFG_TYPE)img.deviceType == S2S_CFG_OPTICAL)
    {
        cdromPerformEject(img);
    }
    else
    {
        doPerformEject(img);
    }
    return true;
}

bool controlInsertMedia(uint8_t scsi_id)
{
    if (scsi_id >= S2S_MAX_TARGETS) return false;
    image_config_t &img = scsiDiskGetImageConfig(scsi_id);
    if (!(img.scsiId & S2S_CFG_TARGET_ENABLED)) return false;
    if (!isRemovableType((S2S_CFG_TYPE)img.deviceType)) return false;

    if (!img.ejected) return true;

    logmsg("Control: inserting media on SCSI ID ", (int)scsi_id);
    if ((S2S_CFG_TYPE)img.deviceType == S2S_CFG_OPTICAL)
        cdromCloseTray(img);
    else
        scsiDiskCloseTray(img);
    return true;
}
