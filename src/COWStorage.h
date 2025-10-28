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

#pragma once

/**
 * Implementation of Copy-on-Write (COW) storage for image backing store.
 */

#include <cstdint>
#include "ZuluSCSI_config.h"	// For ENABLE_COW

#if ENABLE_COW

#include <SdFat.h>
#include "ZuluSCSI_config.h"
#include "ZuluSCSI_settings.h"

 // Copy-on-Write (COW) default configuration
#define DEFAULT_COW_BITMAP_SIZE 4096 // 4KB bitmap = 32768 groups max
#define DEFAULT_COW_BUFFER_SIZE 4096 // 4KB buffer for copy operations

class COWStorage
{
 // Copy-on-Write (COW) members
    FsFile m_fsfile;                 // The real data file.
    FsFile m_fsfile_dirty;           // Overlay file with modified sectors
    uint8_t *m_cow_bitmap;           // Bitmap tracking which groups are dirty
    uint32_t m_bitmap_size;          // Size of bitmap in bytes
    uint32_t m_cow_group_count;      // Total number of groups
    uint32_t m_cow_group_size;       // Size of each group in sectors
    uint32_t m_cow_group_size_bytes; // Size of each group in bytes
    uint32_t m_scsi_block_size_cow;  // SCSI block size for COW operations
    uint64_t m_current_position_cow; // Track current file position for COW

public:
	~COWStorage();

    // Initialize COW storage for the given image file (not in constructor to allow return)
	bool initialize(const char *filename, uint32_t scsi_block_size, scsi_device_settings_t *device_settings);

	//	Clean up COW resources
	void cleanup();

	//	ImageBackingStore functions are redirected here if image is COW
	uint64_t position() const;
    void set_position(uint64_t pos);
	void flush();
	bool isOpen();
    uint64_t size();
	bool seek( uint64_t pos );
    ssize_t read(void *buf, size_t count);
    ssize_t write(const void *buf, size_t count);

private:
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

    // COW internal methods, don't depend on current position
    ssize_t cow_read(uint32_t from, uint32_t to, void *buf);
    ssize_t cow_write(uint32_t from, uint32_t to, const void *buf);

	//	Read from a single image
    ssize_t cow_read_single(uint32_t from, uint32_t count, void *buf);

	//	Perform read from cow, writes to dirty, has to be in same group
	ssize_t performCopyOnWrite(uint32_t from_offset, uint32_t to_offset);
};

#endif // ENABLE_COW
