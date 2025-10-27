/**
 * ZuluSCSI™ - Copyright (c) 2022-2025 Rabbit Hole Computing™
 * Portions - Copyright (C) 2023 Eric Helgeson
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

#include "ImageBackingStore.h"
#include <SdFat.h>
#include <ZuluSCSI_platform.h>
#include "ZuluSCSI.h"
#include "ZuluSCSI_log.h"
#include "ZuluSCSI_config.h"
#include "ZuluSCSI_settings.h"
#include <minIni.h>
#include <strings.h>
#include <string.h>
#include <assert.h>

extern bool g_rawdrive_active;

#if ENABLE_COW

uint8_t *g_cow_buffer;      // Pre-allocated buffer for copy operations
uint32_t g_cow_buffer_size; // Size of COW buffer

static void allocateCOWBuffer()
{
    if (g_cow_buffer == nullptr)
    {
        g_cow_buffer_size = g_scsi_settings.getSystem()->cowBufferSize;
        g_cow_buffer = (uint8_t*)malloc( g_cow_buffer_size );
        assert(g_cow_buffer != nullptr);
    }
}
#endif

ImageBackingStore::ImageBackingStore()
{
    m_iscontiguous = false;
    m_israw = false;
    g_rawdrive_active = m_israw;
    m_isrom = false;
    m_isreadonly_attr = false;
    m_blockdev = nullptr;
    m_bgnsector = m_endsector = m_cursector = 0;
    m_isfolder = false;
    m_foldername[0] = '\0';

#if ENABLE_COW
    // Initialize COW members
    m_iscow = false;
    m_cow_bitmap = nullptr;
    m_bitmap_size = 0;
    m_cow_group_count = 0;
    m_cow_group_size = 0;
    m_cow_group_size_bytes = 0;
    m_scsi_block_size_cow = 0;
    m_current_position_cow = 0;
    m_dirty_filename[0] = '\0';
#endif
}

ImageBackingStore::ImageBackingStore(const char *filename, uint32_t scsi_block_size, scsi_device_settings_t *device_settings) : ImageBackingStore()
{
#if ENABLE_COW
    // Check for Copy-on-Write mode (.cow extension)
    size_t len = strlen(filename);
    if (len > 4 && strcasecmp(filename + len - 4, ".cow") == 0)
    {
        // Generate dirty filename by replacing .cow extension with .tmp
        strncpy(m_dirty_filename, filename, len - 4);
        m_dirty_filename[len - 4] = '\0';
        strcat(m_dirty_filename, ".tmp");

        auto bitmapSize = device_settings->cowBitmapSize;

        if (initializeCOW(filename, m_dirty_filename, bitmapSize, scsi_block_size))
        {
            logmsg("---- COW mode enabled for ", filename, " -> ", m_dirty_filename);
            return; // COW mode successfully enabled
        }
        else
        {
            logmsg("---- COW initialization failed for ", filename, ", falling back to regular mode");
            return;
        }
    }
#endif

    if (strncasecmp(filename, "RAW:", 4) == 0)
    {
        char *endptr, *endptr2;
        m_bgnsector = strtoul(filename + 4, &endptr, 0);
        m_endsector = strtoul(endptr + 1, &endptr2, 0);

        if (*endptr != ':' || *endptr2 != '\0')
        {
            logmsg("Invalid format for raw filename: ", filename);
            return;
        }

        if ((scsi_block_size % SD_SECTOR_SIZE) != 0)
        {
            logmsg("SCSI block size ", (int)scsi_block_size, " is not supported for RAW partitions (must be divisible by 512 bytes)");
            return;
        }

        m_iscontiguous = true;
        m_israw = true;
        g_rawdrive_active = m_israw;
        m_blockdev = SD.card();

        uint32_t sectorCount = SD.card()->sectorCount();
        if (m_endsector >= sectorCount)
        {
            logmsg("---- Limiting RAW image mapping to SD card sector count: ", (int)sectorCount);
            m_endsector = sectorCount - 1;
        }
    }
    else if (strncasecmp(filename, "ROM:", 4) == 0)
    {
        if (!romDriveCheckPresent(&m_romhdr))
        {
            m_romhdr.imagesize = 0;
        }
        else
        {
            m_isrom = true;
        }
    }
    else
    {
        if (SD.open(filename, O_RDONLY).isDir())
        {
            // Folder that contains .cue sheet and multiple .bin files
            m_isfolder = true;
            strncpy(m_foldername, filename, sizeof(m_foldername));
            m_foldername[sizeof(m_foldername)-1] = '\0';
        }
        else
        {
            // Regular image file
            _internal_open(filename);
        }
    }
}

ImageBackingStore::~ImageBackingStore()
{
#if ENABLE_COW
    if (m_iscow)
    {
        dumpStats();
        cleanupCOW();
    }
#endif
}

bool ImageBackingStore::_internal_open(const char *filename)
{
    m_isreadonly_attr = !!(FS_ATTRIB_READ_ONLY & SD.attrib(filename));
    oflag_t open_flag = O_RDWR;
    if (m_isreadonly_attr && !m_isfolder)
    {
        open_flag = O_RDONLY;
        logmsg("---- Image file is read-only, writes disabled");
    }

    if (m_isfolder)
    {
        char fullpath[MAX_FILE_PATH * 2];
        strncpy(fullpath, m_foldername, sizeof(fullpath) - strlen(fullpath));
        strncat(fullpath, "/", sizeof(fullpath) - strlen(fullpath));
        strncat(fullpath, filename, sizeof(fullpath) - strlen(fullpath));
        m_fsfile = SD.open(fullpath, open_flag);
    }
    else
    {
        m_fsfile = SD.open(filename, open_flag);
    }

    if (!m_fsfile.isOpen())
    {
        return false;
    }

    uint32_t sectorcount = m_fsfile.size() / SD_SECTOR_SIZE;
    uint32_t begin = 0, end = 0;
    if (m_fsfile.contiguousRange(&begin, &end) && end >= begin + sectorcount - 1)
    {
        // Convert to raw mapping, this avoids some unnecessary
        // access overhead in SdFat library.
        // If non-aligned offsets are later requested, it automatically falls
        // back to SdFat access mode.
        m_iscontiguous = true;
        m_blockdev = SD.card();
        m_bgnsector = begin;

        if (end != begin + sectorcount)
        {
            uint32_t allocsize = end - begin + 1;
            // Due to issue #80 in ZuluSCSI version 1.0.8 and 1.0.9 the allocated size was mistakenly reported to SCSI controller.
            // If the drive was formatted using those versions, you may have problems accessing it with newer firmware.
            // The old behavior can be restored with setting  [SCSI] UseFATAllocSize = 1 in config file.

            if (g_scsi_settings.getSystem()->useFATAllocSize)
            {
                sectorcount = allocsize;
            }
        }

        m_endsector = begin + sectorcount - 1;
        m_fsfile.flush(); // Note: m_fsfile is also kept open as a fallback.
    }

    return true;
}

bool ImageBackingStore::isOpen()
{
    if (!g_sdcard_present)
    { 
        if (m_isrom)
        {
            return (m_romhdr.imagesize > 0);
        }
        return false;
    }
    else
    {
#if ENABLE_COW
        // Handle Copy-on-Write mode
        if (m_iscow)
        {
            return m_fsfile.isOpen() && m_fsfile_dirty.isOpen();
        }
#endif

        if (m_iscontiguous)
            return (m_blockdev != NULL);
        else if (m_isrom)
            return (m_romhdr.imagesize > 0);
        else if (m_isfolder)
            return m_foldername[0] != '\0';
        else
            return m_fsfile.isOpen();
    }
}

bool ImageBackingStore::isWritable()
{
#if ENABLE_COW
    // COW mode is always writable (writes go to dirty file)
    if (m_iscow)
    {
        return true;
    }
#endif

    return !m_isrom && !m_isreadonly_attr;
}

bool ImageBackingStore::isRaw()
{
    return m_israw;
}

bool ImageBackingStore::isRom()
{
    return m_isrom;
}

bool ImageBackingStore::isFolder()
{
    return m_isfolder;
}

bool ImageBackingStore::isContiguous()
{
    return m_iscontiguous;
}

bool ImageBackingStore::close()
{
#if ENABLE_COW
    if (m_iscow)
    {
        cleanupCOW();
    }
#endif

    m_isfolder = false;
    if (m_iscontiguous)
    {
        m_blockdev = nullptr;
        return true;
    }
    else if (m_isrom)
    {
        m_romhdr.imagesize = 0;
        return true;
    }
    else
    {
        return m_fsfile.close();
    }
}

uint64_t ImageBackingStore::size()
{
#if ENABLE_COW
    // Handle Copy-on-Write mode - return original file size
    if (m_iscow)
    {
        return m_fsfile.size();
    }
#endif

    if (m_iscontiguous && m_blockdev && m_israw)
    {
        return (uint64_t)(m_endsector - m_bgnsector + 1) * SD_SECTOR_SIZE;
    }
    else if (m_isrom)
    {
        return m_romhdr.imagesize;
    }
    else
    {
        return m_fsfile.size();
    }
}

bool ImageBackingStore::contiguousRange(uint32_t* bgnSector, uint32_t* endSector)
{
    if (m_iscontiguous && m_blockdev)
    {
        *bgnSector = m_bgnsector;
        *endSector = m_endsector;
        return true;
    }
    else if (m_isrom)
    {
        *bgnSector = 0;
        *endSector = 0;
        return true;
    }
    else
    {
        return m_fsfile.contiguousRange(bgnSector, endSector);
    }
}

bool ImageBackingStore::seek(uint64_t pos)
{
#if ENABLE_COW
    // Handle Copy-on-Write mode
    if (m_iscow)
    {
        m_current_position_cow = pos;
        return true; // COW always supports arbitrary seeking
    }
#endif

    uint32_t sectornum = pos / SD_SECTOR_SIZE;

    if (m_iscontiguous && (uint64_t)sectornum * SD_SECTOR_SIZE != pos)
    {
        dbgmsg("---- Unaligned access to image, falling back to SdFat access mode");
        m_iscontiguous = false;
    }

    if (m_iscontiguous)
    {
        m_cursector = m_bgnsector + sectornum;
        return (m_cursector <= m_endsector);
    }
    else if (m_isrom)
    {
        uint32_t sectornum = pos / SD_SECTOR_SIZE;
        assert((uint64_t)sectornum * SD_SECTOR_SIZE == pos);
        m_cursector = sectornum;
        return m_cursector * SD_SECTOR_SIZE < m_romhdr.imagesize;
    }
    else
    {
        return m_fsfile.seek(pos);
    }
}

ssize_t ImageBackingStore::read(void* buf, size_t count)
{
#if ENABLE_COW
    // Handle Copy-on-Write mode
    if (m_iscow)
    {
        return cow_read(buf, count);
    }
#endif

    uint32_t sectorcount = count / SD_SECTOR_SIZE;
    if (m_iscontiguous && (uint64_t)sectorcount * SD_SECTOR_SIZE != count)
    {
        dbgmsg("---- Unaligned access to image, falling back to SdFat access mode");
        m_iscontiguous = false;
    }

    if (m_iscontiguous && m_blockdev)
    {
        if (m_blockdev->readSectors(m_cursector, (uint8_t*)buf, sectorcount))
        {
            m_cursector += sectorcount;
            return count;
        }
        else
        {
            return -1;
        }
    }
    else if (m_isrom)
    {
        uint32_t sectorcount = count / SD_SECTOR_SIZE;
        assert((uint64_t)sectorcount * SD_SECTOR_SIZE == count);
        uint32_t start = m_cursector * SD_SECTOR_SIZE;
        if (romDriveRead((uint8_t*)buf, start, count))
        {
            m_cursector += sectorcount;
            return count;
        }
        else
        {
            return -1;
        }
    }
    else
    {
        return m_fsfile.read(buf, count);
    }
}

ssize_t ImageBackingStore::write(const void* buf, size_t count)
{
#if ENABLE_COW
    // Handle Copy-on-Write mode
    if (m_iscow)
    {
        return cow_write(buf, count);
    }
#endif

    uint32_t sectorcount = count / SD_SECTOR_SIZE;
    if (m_iscontiguous && (uint64_t)sectorcount * SD_SECTOR_SIZE != count)
    {
        dbgmsg("---- Unaligned access to image, falling back to SdFat access mode");
        m_iscontiguous = false;
    }

    if (m_iscontiguous && m_blockdev)
    {
        if (m_blockdev->writeSectors(m_cursector, (const uint8_t*)buf, sectorcount))
        {
            m_cursector += sectorcount;
            return count;
        }
        else
        {
            return 0;
        }
    }
    else if (m_isrom)
    {
        logmsg("ERROR: attempted to write to ROM drive");
        return 0;
    }
    else  if (m_isreadonly_attr)
    {
        logmsg("ERROR: attempted to write to a read only image");
        return 0;
    }
    else
    {
        return m_fsfile.write(buf, count);
    }
}

void ImageBackingStore::flush()
{
#if ENABLE_COW
    // Handle Copy-on-Write mode
    if (m_iscow)
    {
        m_fsfile_dirty.flush();
        return;
    }
#endif

    if (!m_iscontiguous && !m_isrom && !m_isreadonly_attr)
    {
        m_fsfile.flush();
    }
}

uint64_t ImageBackingStore::position()
{
#if ENABLE_COW
    // Handle Copy-on-Write mode
    if (m_iscow)
    {
        return m_current_position_cow;
    }
#endif

    if (!m_iscontiguous && !m_isrom)
    {
        return m_fsfile.curPosition();
    }
    else
    {
        return 0;
    }
}

size_t ImageBackingStore::getFilename(char* buf, size_t buflen)
{
    if (m_fsfile.isOpen())
    {
        size_t name_length = m_fsfile.getName(buf, buflen);
        if (name_length + 1 > buflen)
            return 0;
        else
            return name_length;
    }
    return 0;
}

bool ImageBackingStore::selectImageFile(const char *filename)
{
    if (!m_isfolder)
    {
        logmsg("Attempted selectImageFile() but image is not a folder");
        return false;
    }
    return _internal_open(filename);
}

size_t ImageBackingStore::getFoldername(char* buf, size_t buflen)
{
    if (m_isfolder)
    {
        size_t name_length = strlen(m_foldername);
        if (name_length + 1 > buflen)
            return 0;

        strncpy(buf, m_foldername, buflen);
        return name_length;
    }

    return 0;
}

#if ENABLE_COW

//=============================================================================
// Copy-on-Write (COW) Implementation
//=============================================================================

static bool createDirtyFile(const char *dirty_filename, uint64_t size)
{
    //  If file already exists and is same size or larger, do nothing
    FsFile file;
    file.open(dirty_filename, O_RDONLY);
    if (file.isOpen()) // file exists
    {
        uint64_t existing_size = file.size();
        file.close();
        if (existing_size >= size) // size ok
        {
            return true;
        }
        logmsg("---- Dirty file exists but is too small, recreating: ", dirty_filename);
        SD.remove(dirty_filename);
    }

    return createImageFile(dirty_filename, size);
}

// Initializes copy-on-write store: bitmap_size (dirty tracking), buffer_size (I/O chunks), scsi_block_size (sector size)
bool ImageBackingStore::initializeCOW(const char *orig_filename, const char *dirty_filename,
                                      uint32_t bitmap_max_size, uint32_t scsi_block_size)
{
    logmsg("---- Initializing COW for image: ", orig_filename);
    logmsg("---- Bitmap max size: ", (int)bitmap_max_size, " bytes");

    allocateCOWBuffer();

    m_iscow = true;

    m_scsi_block_size_cow = scsi_block_size;
    m_current_position_cow = 0;

    // Open files
    m_fsfile.open(orig_filename, O_RDONLY);
    if (!createDirtyFile(dirty_filename, m_fsfile.size()))
    {
        logmsg("---- COW initialization failed: could not create dirty file");
        return false;
    }
    m_fsfile_dirty.open(dirty_filename, O_RDWR);

    // Calculate image size in sectors
    uint64_t image_size_bytes = m_fsfile.size();
    uint32_t total_sectors = image_size_bytes / scsi_block_size;

    // Create overlay file at the same size as original (sparse)
    m_fsfile_dirty.seek(image_size_bytes - 1);
    uint8_t zero = 0;
    ssize_t written = m_fsfile_dirty.write(&zero, 1); // Create sparse file of correct size
    if (written != 1)
    {
        logmsg("---- COW initialization failed: dirty file write error");
        return false;
    }

    //  We may need to repeat the calculations if the bitmap size if too large to be allocated
    do
    {
        // Calculate optimal group size based on provided bitmap size
        // We have bitmap_size * 8 bits available in our bitmap
        uint32_t max_groups = bitmap_max_size * 8;

        // Calculate group size - must be multiple of 512 sectors and fit within bitmap
        m_cow_group_size = ((total_sectors + max_groups - 1) / max_groups);
        m_cow_group_size_bytes = m_cow_group_size * m_scsi_block_size_cow;

        // Calculate actual number of groups needed
        m_cow_group_count = (total_sectors + m_cow_group_size - 1) / m_cow_group_size;

        // This should never happen due to our group size calculation
        assert(m_cow_group_count <= max_groups);

        m_bitmap_size = (m_cow_group_count + 7) / 8;

        // Allocate and initialize bitmap using the provided bitmap_size
        //  On RP2040, 'new uint8_t[]' returns a pointer even when the allocation failed
        // m_cow_bitmap = new uint8_t[m_bitmap_size];
        m_cow_bitmap = (uint8_t *)malloc( m_bitmap_size );

        if (!m_cow_bitmap)
        {
            //  Out of memory
            if (bitmap_max_size < 128)
            {
                logmsg("---- COW initialization failed: memory too low");
                return false;
            }

            // Try again with smaller bitmap size
            bitmap_max_size /= 2;
            logmsg("---- COW bitmap allocation of ", (int)m_bitmap_size, " bytes failed, trying max size of ", (int)bitmap_max_size, " bytes");
        }
        else
        {
            memset(m_cow_bitmap, 0, m_bitmap_size);
        }
    } while (!m_cow_bitmap);

    logmsg("---- COW image size: ", (int)image_size_bytes, " bytes");
    logmsg("---- COW bitmap: ", (int)m_cow_group_count, " groups, ", (int)m_bitmap_size, " bytes (requested: ", (int)bitmap_max_size, ")");
    logmsg("---- COW group size: ", (int)m_cow_group_size, " sectors (", (int)m_cow_group_size_bytes, " bytes)");
    logmsg("---- COW block size: ", (int)m_scsi_block_size_cow, " bytes");
    logmsg("---- COW buffer size: ", (int)g_cow_buffer_size, " bytes");

    resetStats();

    return true;
}

void ImageBackingStore::resetStats()
{
    m_bytes_written_dirty = 0;
    m_bytes_requested_write = 0;
    m_bytes_read_original_cow = 0;
}

void ImageBackingStore::dumpStats()
{
    if (m_iscow && m_bytes_requested_write > 0)
    {
        int over_write = 100.0 * (static_cast<double>(m_bytes_read_original_cow + m_bytes_written_dirty) / m_bytes_requested_write - 1);
        char name[MAX_FILE_PATH];
        m_fsfile.getName(name, sizeof(name));
        dbgmsg( name, ": write overhead : ", over_write, "% (", (int)m_bytes_read_original_cow, " bytes of original read + ", (int)m_bytes_written_dirty, " bytes of dirty write ) / ", (int)m_bytes_requested_write, " bytes of write");
    }
}

void ImageBackingStore::set_position(uint64_t pos)
{
    m_current_position_cow = pos;
}

void ImageBackingStore::cleanupCOW()
{
    if (m_cow_bitmap)
    {
        // delete[] m_cow_bitmap;
        free( m_cow_bitmap );
        m_cow_bitmap = nullptr;
    }
    if (m_fsfile_dirty.isOpen())
    {
        m_fsfile_dirty.close();
    }
    m_iscow = false;
}

// Returns whether a group is stored in original or dirty file by checking bitmap
ImageBackingStore::COWImageType ImageBackingStore::getGroupImageType(uint32_t group)
{
    assert(group < m_cow_group_count);
    return (m_cow_bitmap[group / 8] & (1 << (group % 8))) ? COW_IMG_TYPE_DIRTY : COW_IMG_TYPE_ORIG;
}

// Sets group type in bitmap by setting or clearing the corresponding bit
void ImageBackingStore::setGroupImageType(uint32_t group, COWImageType type)
{
    assert(group < m_cow_group_count);
    if (type == COW_IMG_TYPE_DIRTY)
    {
        m_cow_bitmap[group / 8] |= (1 << (group % 8));
    }
    else
    {
        m_cow_bitmap[group / 8] &= ~(1 << (group % 8));
    }
}

// Reads from a single image type (original or dirty) for given byte range
// Used for implementation the high-level read
ssize_t ImageBackingStore::cow_read_single(uint32_t from, uint32_t count, void *buf)
{
    if (getGroupImageType(groupFromOffset(from)) == COW_IMG_TYPE_DIRTY)
    {
        // Read from overlay/dirty file at same offset as original
        m_fsfile_dirty.seek(from);
        return m_fsfile_dirty.read(buf, count);
    }
    // Read from original file
    m_fsfile.seek(from);
    return m_fsfile.read(buf, count);
}

/*
    Reads across multiple groups, switching between original and dirty files as needed

|--------|--------|--------|--------|--------|--------|--------|--------|--------|--------|--------|--------|----- Sectors (512 bytes each)
                           |                          |                          |                          |      Groups (3 sectors each)
           DIRTY           |          CLEAN           |          CLEAN           |          DIRTY           |      Group state before write
          [---------------------------------------------------------------------------------------]         |      Read 10 blocs, spanning 4 groups
          [  DIRTY READ   ] [                      CLEAN READ                   ] [  DIRTY READ   ]|        |      Underlying chunks from alternating sources
|--------|--------|--------|--------|--------|--------|--------|--------|--------|--------|--------|--------|----- Sectors (512 bytes each)

    Idea is we repeatedly create a "chunk" that extends from the current read position
    to the next transition between original and dirty, or to the end of the read request
*/
ssize_t ImageBackingStore::cow_read(uint32_t from, uint32_t to, void *buf)
{
    ssize_t total_bytes_read = 0;
    uint8_t *buffer_ptr = static_cast<uint8_t *>(buf);
    uint32_t current_offset = from;

    while (current_offset < to)
    {
        // Find the end of the current chunk (either 'to' or where image type changes)
        uint32_t current_group = groupFromOffset(current_offset);
        COWImageType current_type = getGroupImageType(current_group);
        uint32_t chunk_end = current_offset;

        // Extend chunk while image type remains the same and we haven't reached 'to'
        while (chunk_end < to && groupFromOffset(chunk_end) < m_cow_group_count &&
               getGroupImageType(groupFromOffset(chunk_end)) == current_type)
        {
            uint32_t next_group_offset = offsetFromGroup(groupFromOffset(chunk_end) + 1);
            chunk_end = (to < next_group_offset) ? to : next_group_offset;
        }

        // Read this chunk using cow_read_single
        ssize_t bytes_read = cow_read_single(current_offset, chunk_end - current_offset, buffer_ptr);
        if (bytes_read <= 0)
            break;

        total_bytes_read += bytes_read;
        buffer_ptr += bytes_read;
        current_offset = chunk_end;
    }

    return total_bytes_read;
}

// Wrapper for cow_read that uses current file position and updates it
ssize_t ImageBackingStore::cow_read(void *buf, size_t count)
{
    uint32_t from = static_cast<uint32_t>(m_current_position_cow);
    uint32_t to = from + static_cast<uint32_t>(count);

    ssize_t bytes_read = cow_read(from, to, buf);

    // Update current position
    if (bytes_read > 0)
    {
        set_position(m_current_position_cow + bytes_read);
    }

    return bytes_read;
}

//  Helper function for cow_write
//  Copies original data to overlay (dirty) file for a specific byte range
//  Request never spans multiple groups
ssize_t ImageBackingStore::performCopyOnWrite(uint32_t from_offset, uint32_t to_offset)
{
    // Verify both offsets are in the same group
    assert(groupFromOffset(from_offset) == groupFromOffset(to_offset - 1));

    uint32_t bytes_to_copy = to_offset - from_offset;
    uint32_t bytes_copied = 0;

    m_fsfile.seek(from_offset);
    m_fsfile_dirty.seek(from_offset);

    while (bytes_copied < bytes_to_copy)
    {
        uint32_t chunk_size = (g_cow_buffer_size < (bytes_to_copy - bytes_copied)) ? g_cow_buffer_size : (bytes_to_copy - bytes_copied);

        ssize_t bytes_read = m_fsfile.read(g_cow_buffer, chunk_size);
        if (bytes_read < 0)
        {
            logmsg("COW read error during copy-on-write operation");
            return bytes_read; // Return read error immediately
        }
        if (static_cast<uint32_t>(bytes_read) != chunk_size)
        {
            logmsg("COW unexpected partial read during copy-on-write operation");
            return -1; // Unexpected partial read
        }
        m_bytes_read_original_cow += chunk_size;

        ssize_t bytes_written = m_fsfile_dirty.write(g_cow_buffer, chunk_size);
        if (bytes_written < 0)
        {
            logmsg("COW write error during copy-on-write operation");
            return bytes_written; // Return write error immediately
        }
        if (static_cast<uint32_t>(bytes_written) != chunk_size)
        {
            logmsg("COW unexpected partial write during copy-on-write operation");
            return -1; // Unexpected partial write
        }
        m_bytes_written_dirty += chunk_size;

        bytes_copied += chunk_size;
    }

    return bytes_to_copy; // Return total bytes copied
}

/*
    Writes data performing copy-on-write for unmodified portions at begining and end

|--------|--------|--------|--------|--------|--------|--------|--------|--------|--------|--------|----- Sectors (512 bytes each)
                  |                          |                          |                          |      Groups (3 sectors each)
  CLEAN           |          CLEAN           |          CLEAN           |          CLEAN           |      Group state before write
                  |                  [---------------------------------------------------]         |      Write 6 blocs, spanning 3 groups
                  |[ COPY...COPY...] [ WRITE...WRITE...WRITE...WRITE...WRITE...WRITE...  ] [ COPY ]|      Actions taken (1), (2), (3)
  CLEAN           |          DIRTY           |          DIRTY           |          DIRTY           |      Group market dirty after write (4)
|--------|--------|--------|--------|--------|--------|--------|--------|--------|--------|--------|----- Sectors (512 bytes each)

    Implementation follows the above pattern:
    - (1) Handle first group: if clean and write doesn't start at group beginning, copy original data
    - (2) Write the main data to dirty file
    - (3) Handle last group: if clean and write doesn't end at group end, copy original data
    - (4) Mark all affected groups as dirty
*/
ssize_t ImageBackingStore::cow_write(uint32_t from, uint32_t to, const void *buf)
{
    size_t count = to - from;

    uint32_t first_group = groupFromOffset(from);
    uint32_t last_group = groupFromOffset(to - 1); // Last byte affected

    // Handle first group - copy-on-write if needed and write doesn't start at group beginning
    if (getGroupImageType(first_group) == COW_IMG_TYPE_ORIG)
    {
        uint32_t group_start = offsetFromGroup(first_group);
        if (from > group_start)
        {
            // Need to preserve data before the write
            ssize_t cow_result = performCopyOnWrite(group_start, from);
            if (cow_result < 0)
            {
                return cow_result; // Return COW error immediately
            }
        }
    }

    // Handle copy in the dirty file
    m_fsfile_dirty.seek(from);
    ssize_t bytes_written = m_fsfile_dirty.write(buf, count);
    if (bytes_written <= 0)
    {
        return bytes_written;
    }

    m_bytes_written_dirty += count;

    // Handle last group - copy-on-write if needed and write doesn't end at group end
    if (getGroupImageType(last_group) == COW_IMG_TYPE_ORIG)
    {
        uint32_t group_end = offsetFromGroup(last_group + 1);
        if (to < group_end)
        {
            // Need to preserve data after the write
            ssize_t cow_result = performCopyOnWrite(to, group_end);
            if (cow_result < 0)
            {
                return cow_result; // Return COW error immediately
            }
        }
    }

    // Mark all affected groups as dirty
    for (uint32_t group = first_group; group <= last_group; ++group)
    {
        setGroupImageType(group, COW_IMG_TYPE_DIRTY);
    }

    return bytes_written;
}

// Public wrapper for cow_write (that uses current file position and updates it)
ssize_t ImageBackingStore::cow_write(const void *buf, size_t count)
{
    m_bytes_requested_write += count;

    uint32_t from = static_cast<uint32_t>(m_current_position_cow);
    uint32_t to = from + static_cast<uint32_t>(count);

    ssize_t bytes_written = cow_write(from, to, buf);

    // Update current position (unsure if needed)
    if (bytes_written > 0)
    {
        set_position(m_current_position_cow + bytes_written);
    }

    //  Every mega bytes, we dump current stats
    if (m_bytes_requested_write > 1000000)
    {
        dumpStats();
        resetStats();
    }

    return bytes_written;
}

#endif // ENABLE_COW
