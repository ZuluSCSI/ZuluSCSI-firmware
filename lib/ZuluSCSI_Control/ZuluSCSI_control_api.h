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

// Interface-neutral media management API.
// Provides image selection, listing, ejection, and insertion for removable
// SCSI devices (CD-ROM, Tape, Removable, Floppy, MO, Zip).
// Usable by the USB serial console, future network interfaces, or other UIs.

#pragma once

#include <stdint.h>
#include <stddef.h>

// Check if a SCSI target has removable/changeable media
bool controlIsRemovableDevice(uint8_t scsi_id);

// List all active removable device SCSI IDs into ids_out[0..max_count-1]
// Returns number of devices found
int controlListRemovableDevices(uint8_t *ids_out, int max_count);

// Human-readable device type name for display (e.g. "CD-ROM", "Tape")
const char* controlGetDeviceTypeName(uint8_t scsi_id);

// Get the full path of the currently mounted image for a device.
// Returns true if an image is loaded (not ejected); buf receives the path.
// Returns false and sets buf[0]='\0' when ejected or device inactive.
bool controlGetCurrentImage(uint8_t scsi_id, char *buf, size_t buflen);

// Get the directory that is searched for available images for a device.
// Returns true and writes the directory path into buf on success.
// Returns false when no image directory is configured.
bool controlGetImageDirectory(uint8_t scsi_id, char *buf, size_t buflen);

// Enumerate available images in the device's image directory.
// For each image found, calls:
//   callback(idx, filename, full_path, size_bytes, is_dir, userdata)
// CUE/BIN folder sets are surfaced as single entries (full_path is the folder,
// is_dir=true, size=0).  Plain image files have is_dir=false.
// Returns the total count of images found.
int controlListImages(uint8_t scsi_id,
    void (*callback)(int idx, const char *filename,
                     const char *full_path, uint64_t size, bool is_dir, void *userdata),
    void *userdata);

// Load (mount) a specific image on a device.
// full_path is the absolute SD card path returned by controlListImages.
// Pass nullptr to advance to the next image in the device's sequence.
// Ejects current media and signals a media change to the host.
// Returns true on success.
bool controlLoadImage(uint8_t scsi_id, const char *full_path);

// Eject current media from a device (host sees the drive as empty).
// Returns true on success; true is also returned if already ejected.
bool controlEjectMedia(uint8_t scsi_id);

// Insert / close tray — makes the currently staged media accessible again.
// Returns true on success; true is also returned if already inserted.
bool controlInsertMedia(uint8_t scsi_id);
