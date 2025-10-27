/**
 * Portions - Copyright (C) 2023 Eric Helgeson
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details. 
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
**/

/* Access layer to image files associated with a SCSI device.
 * Currently supported image storage modes:
 *
 * - Files on SD card
 * - Raw SD card partitions
 * - Microcontroller flash ROM drive
 */

#pragma once
#include <stdint.h>
#include <unistd.h>
#include <SdFat.h>
#include "ROMDrive.h"
#include "ZuluSCSI_config.h"
#include "ZuluSCSI_settings.h"

extern "C" {
#include <scsi.h>
}

// SD card sector size is always 512 bytes
extern SdFs SD;
#define SD_SECTOR_SIZE 512


#if ENABLE_COW
// Copy-on-Write (COW) default configuration
#define DEFAULT_COW_BITMAP_SIZE 4096 // 4KB bitmap = 32768 groups max
#define DEFAULT_COW_BUFFER_SIZE 4096 // 4KB buffer for copy operations
#endif

// This class wraps SdFat library FsFile to allow access
// through either FAT filesystem or as a raw sector range.
//
// Raw access is activated by using filename like "RAW:0:12345"
// where the numbers are the first and last sector.
//
// If the platform supports a ROM drive, it is activated by using
// filename "ROM:".
class ImageBackingStore
{
public:
    // Empty image, cannot be accessed
    ImageBackingStore();

    // Parse image file parameters from filename.
    // Special filename formats:
    //    RAW:start:end
    //    ROM:
    //    *.cow (enables copy-on-write)
    ImageBackingStore(const char *filename, uint32_t scsi_block_size, scsi_device_settings_t *device_config);

    // Destructor to clean up COW resources
    ~ImageBackingStore();

    // Disable copy and move operations entirely
    ImageBackingStore(const ImageBackingStore &) = delete;
    ImageBackingStore &operator=(const ImageBackingStore &) = delete;
    ImageBackingStore(ImageBackingStore &&) = delete;
    ImageBackingStore &operator=(ImageBackingStore &&) = delete;

    // Can the image be read?
    bool isOpen();

    // Can the image be written?
    bool isWritable();

    // Is this in Raw mode? by passing the file system
    bool isRaw();

    // Is this internal ROM drive in microcontroller flash?
    bool isRom();

    // Is the image a folder, which contains multiple files (used for .bin/.cue)
    bool isFolder();

    // Is this a contigious block on the SD card? Allowing less overhead
    bool isContiguous();

    // Close the image so that .isOpen() will return false.
    bool close();

    // Return image size in bytes
    uint64_t size();

    // Check if the image sector range is contiguous, and the image is on
    // SD card, return the sector numbers.
    bool contiguousRange(uint32_t* bgnSector, uint32_t* endSector);

    // Set current position for following read/write operations
    bool seek(uint64_t pos);

    // Read data from the image file, returns number of bytes read, or negative on error.
    ssize_t read(void* buf, size_t count);

    // Write data to image file, returns number of bytes written, or negative on error.
    ssize_t write(const void* buf, size_t count);

    // Flush any pending changes to filesystem
    void flush();

    // Gets current position for following read/write operations
    // Result is only valid for regular files, not raw or flash access
    uint64_t position();

    size_t getFilename(char* buf, size_t buflen);

    // Change image if the image is a folder (used for .cue with multiple .bin)
    bool selectImageFile(const char *filename);
    size_t getFoldername(char* buf, size_t buflen);

protected:
    bool m_iscontiguous;
    bool m_israw;
    bool m_isrom;
    bool m_isreadonly_attr;
    romdrive_hdr_t m_romhdr;
    FsFile m_fsfile;
    SdCard *m_blockdev;
    uint32_t m_bgnsector;
    uint32_t m_endsector;
    uint32_t m_cursector;

    bool m_isfolder;
    char m_foldername[MAX_FILE_PATH + 1];

    bool _internal_open(const char *filename);

#if ENABLE_COW
    // Copy-on-Write (COW) members
    bool m_iscow;

    FsFile m_fsfile_dirty;           // Overlay file with modified sectors
    uint8_t *m_cow_bitmap;           // Bitmap tracking which groups are dirty
    uint32_t m_bitmap_size;          // Size of bitmap in bytes
    uint32_t m_cow_group_count;      // Total number of groups
    uint32_t m_cow_group_size;       // Size of each group in sectors
    uint32_t m_cow_group_size_bytes; // Size of each group in bytes
    uint32_t m_scsi_block_size_cow;  // SCSI block size for COW operations
    uint64_t m_current_position_cow; // Track current file position for COW

    // Statistics counters
    mutable uint64_t m_bytes_written_dirty = 0;     // Bytes written to dirty file
    mutable uint64_t m_bytes_requested_write = 0;   // Bytes requested to be written by public methods
    mutable uint64_t m_bytes_read_original_cow = 0; // Bytes read from original file due to COW operations

    // COW helper methods
    bool initializeCOW(const char *orig_filename, const char *dirty_filename, uint32_t bitmap_max_size, uint32_t scsi_block_size);
    void cleanupCOW();
    ssize_t cow_read(void *buf, size_t count);
    ssize_t cow_write(const void *buf, size_t count);

    // COW internal methods (overloads)
    ssize_t cow_read(uint32_t from, uint32_t to, void *buf);
    ssize_t cow_write(uint32_t from, uint32_t to, const void *buf);
    ssize_t cow_read_single(uint32_t from, uint32_t count, void *buf);
    void set_position(uint64_t pos);

    // COW statistics
    void resetStats();
    void dumpStats();

    // COW bitmap management
    enum COWImageType
    {
        COW_IMG_TYPE_ORIG = 0,
        COW_IMG_TYPE_DIRTY = 1
    };
    COWImageType getGroupImageType(uint32_t group);
    void setGroupImageType(uint32_t group, COWImageType type);
    uint32_t groupFromOffset(uint32_t offset) { return offset / m_cow_group_size / m_scsi_block_size_cow; }
    uint32_t offsetFromGroup(uint32_t group) { return group * m_cow_group_size * m_scsi_block_size_cow; }
    ssize_t performCopyOnWrite(uint32_t from_offset, uint32_t to_offset);
#endif
};
