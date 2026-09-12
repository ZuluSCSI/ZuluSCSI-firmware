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
#include "ZuluSCSI_iotrace.h"
#include <minIni.h>
#include <strings.h>
#include <string.h>
#include <assert.h>

extern bool g_rawdrive_active;

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
#endif
}

ImageBackingStore::ImageBackingStore(const char *filename, uint32_t scsi_block_size, scsi_device_settings_t *device_settings, uint8_t scsiId) : ImageBackingStore()
{
    m_iotraceScsiId = scsiId;

#if ENABLE_COW
    if (m_cow.initialize(filename, scsi_block_size, device_settings))
    {
        m_iscow = true;
        return; // COW mode successfully enabled
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
#ifdef CONTAINER_IMAGE_SUPPORT
        if (m_fsfile.isUnsupportedContainerType())
        {
            logmsg("============ ERROR: Unsupported container image type ============");
            logmsg("Image is a ", m_fsfile.getContainerNameCstr(), " container but the image type is unsupported.");
            logmsg("Please use a container with a fixed size or fully allocated image");
            logmsg("=================================================================");
        }
#endif
        return false;
    }

    uint32_t sectorcount = m_fsfile.size() / SD_SECTOR_SIZE;
    uint32_t begin = 0, end = 0;
    bool got_range = m_fsfile.contiguousRange(&begin, &end);
    bool range_covers_file = got_range && end >= begin + sectorcount - 1;

    // Diagnostic for the AS/400 performance investigation (see project
    // memory: project_as400_write_performance.md) -- logged unconditionally
    // at open time, independent of IOTrace, since this is the actual root
    // decision point for whether ImageBackingStore's raw-block fast path
    // is even reachable for this file at all. A trace showing 0% fast-path
    // accesses only tells you the symptom; this tells you why, immediately,
    // without needing a trace to infer it from.
    if (!got_range)
    {
        logmsg("---- ", filename, ": not contiguous on SD card (contiguousRange() failed) -- ",
               "ImageBackingStore's raw-block fast path is unavailable for this file");
    }
    else if (!range_covers_file)
    {
        logmsg("---- ", filename, ": contiguous range too short for fast path -- have sectors ",
               (int)begin, "-", (int)end, " (", (int)(end - begin + 1), "), need ", (int)sectorcount);
    }
    else
    {
        logmsg("---- ", filename, ": contiguous sectors ", (int)begin, "-", (int)end,
               " -- fast path available");
    }

    uint8_t iotrace_imageopen_flags = (got_range ? IOTRACE_IMAGEOPEN_FLAG_GOT_RANGE : 0) |
        (range_covers_file ? IOTRACE_IMAGEOPEN_FLAG_CONTIGUOUS : 0);
    iotrace_imageopen(m_iotraceScsiId, got_range ? begin : 0, got_range ? end : 0, sectorcount, iotrace_imageopen_flags);

    if (range_covers_file)
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

void ImageBackingStore::revert_to_noncontiguous()
{
    if (m_iscontiguous && !m_israw && m_fsfile.isOpen())
    {
        // Revert from direct SD card access to filesystem based access.
        // Keep the seek position.
        m_iscontiguous = false;
        m_fsfile.seek((m_cursector - m_bgnsector) * SD_SECTOR_SIZE);
    }
}

bool ImageBackingStore::isOpen()
{
#if ENABLE_COW
    if (m_iscow)
    {
        return m_cow.isOpen();
    }
#endif
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
#if ENABLE_COW
    if (m_iscow)
    {
        return false;
    }
#endif
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
        m_cow.cleanup();
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
        return m_cow.size();
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
        return m_cow.seek( pos );
    }
#endif

    uint32_t sectornum = pos / SD_SECTOR_SIZE;

    if (m_iscontiguous && (uint64_t)sectornum * SD_SECTOR_SIZE != pos)
    {
        dbgmsg("---- Unaligned access to image, falling back to SdFat access mode");
        revert_to_noncontiguous();
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
        return m_cow.read(buf, count);
    }
#endif

    bool iotrace_was_contiguous = m_iscontiguous;
    uint32_t sectorcount = count / SD_SECTOR_SIZE;
    if (m_iscontiguous && (uint64_t)sectorcount * SD_SECTOR_SIZE != count)
    {
        dbgmsg("---- Unaligned access to image, falling back to SdFat access mode");
        revert_to_noncontiguous();
    }

    // IOTrace Layer B: sector/flags captured now, before m_cursector
    // advances or m_fsfile's position moves past this access -- the actual
    // logging call happens after each dispatch branch below, once the
    // real operation duration is known, so it can be carried on the same
    // record instead of only being visible pooled into Layer C's DMA_WAIT
    // bucket. `sector` is approximate once non-contiguous (SdFat's own
    // file position expressed in 512-byte units, not this image's real
    // sector size), but still monotonic/comparable across log entries.
    // No-op entirely when IOTrace= is off. Not logged at all for the ROM
    // branch below -- that's flash-memory access, not SD card I/O.
    uint32_t iotrace_sector = m_iscontiguous ? m_cursector : (uint32_t)(m_fsfile.position() / SD_SECTOR_SIZE);
    uint8_t iotrace_flags = (m_iscontiguous ? IOTRACE_SD_FLAG_CONTIGUOUS : 0) |
        ((iotrace_was_contiguous && !m_iscontiguous) ? IOTRACE_SD_FLAG_REVERTED : 0);

    if (m_iscontiguous && m_blockdev)
    {
        // IOTrace Layer C: this call (and the equivalent m_fsfile.read()
        // below) is where the CPU actually blocks waiting for the SD card
        // -- timing it directly here is simpler and lower-risk than
        // reaching into sdio.cpp's own low-level DMA-completion wait
        // loops. Bucket name says DMA_WAIT but really means "blocked on
        // storage I/O," covering both this raw path and the SdFat
        // fallback below.
        uint64_t iotrace_t0 = iotrace_now_us();
        bool ok = m_blockdev->readSectors(m_cursector, (uint8_t*)buf, sectorcount);
        uint32_t iotrace_duration_us = (uint32_t)(iotrace_now_us() - iotrace_t0);
        iotrace_loop_account(IOTRACE_BUCKET_DMA_WAIT, iotrace_duration_us);
        iotrace_sd_access(m_iotraceScsiId, iotrace_sector, (uint16_t)sectorcount, iotrace_flags, iotrace_duration_us);
        if (ok)
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
        uint64_t iotrace_t0 = iotrace_now_us();
        ssize_t result = m_fsfile.read(buf, count);
        uint32_t iotrace_duration_us = (uint32_t)(iotrace_now_us() - iotrace_t0);
        iotrace_loop_account(IOTRACE_BUCKET_DMA_WAIT, iotrace_duration_us);
        iotrace_sd_access(m_iotraceScsiId, iotrace_sector, (uint16_t)sectorcount, iotrace_flags, iotrace_duration_us);
        return result;
    }
}

ssize_t ImageBackingStore::write(const void* buf, size_t count)
{
    #if ENABLE_COW
    // Handle Copy-on-Write mode
    if (m_iscow)
    {
        return m_cow.write(buf, count);
    }
#endif

    bool iotrace_was_contiguous = m_iscontiguous;
    uint32_t sectorcount = count / SD_SECTOR_SIZE;
    if (m_iscontiguous && (uint64_t)sectorcount * SD_SECTOR_SIZE != count)
    {
        dbgmsg("---- Unaligned access to image, falling back to SdFat access mode");
        revert_to_noncontiguous();
    }

    // IOTrace Layer B: see the matching comment in read() above -- same
    // reasoning (logged after dispatch, with the real duration, not at
    // each return point below except where dispatch actually occurred).
    uint32_t iotrace_sector = m_iscontiguous ? m_cursector : (uint32_t)(m_fsfile.position() / SD_SECTOR_SIZE);
    uint8_t iotrace_flags = IOTRACE_SD_FLAG_WRITE |
        (m_iscontiguous ? IOTRACE_SD_FLAG_CONTIGUOUS : 0) |
        ((iotrace_was_contiguous && !m_iscontiguous) ? IOTRACE_SD_FLAG_REVERTED : 0);

    if (m_iscontiguous && m_blockdev)
    {
        // IOTrace Layer C: see the matching comment in read() above.
        uint64_t iotrace_t0 = iotrace_now_us();
        bool ok = m_blockdev->writeSectors(m_cursector, (const uint8_t*)buf, sectorcount);
        uint32_t iotrace_duration_us = (uint32_t)(iotrace_now_us() - iotrace_t0);
        iotrace_loop_account(IOTRACE_BUCKET_DMA_WAIT, iotrace_duration_us);
        iotrace_sd_access(m_iotraceScsiId, iotrace_sector, (uint16_t)sectorcount, iotrace_flags, iotrace_duration_us);
        if (ok)
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
        uint64_t iotrace_t0 = iotrace_now_us();
        ssize_t result = m_fsfile.write(buf, count);
        uint32_t iotrace_duration_us = (uint32_t)(iotrace_now_us() - iotrace_t0);
        iotrace_loop_account(IOTRACE_BUCKET_DMA_WAIT, iotrace_duration_us);
        iotrace_sd_access(m_iotraceScsiId, iotrace_sector, (uint16_t)sectorcount, iotrace_flags, iotrace_duration_us);
        return result;
    }
}

void ImageBackingStore::flush()
{
#if ENABLE_COW
    // Handle Copy-on-Write mode
    if (m_iscow)
    {
        m_cow.flush();
        return;
    }
#endif

    if (!m_iscontiguous && !m_isrom && !m_isreadonly_attr)
    {
        m_fsfile.flush();
    }
}

bool ImageBackingStore::truncate(uint64_t size)
{
    if (m_isrom || m_israw || m_isreadonly_attr)
    {
        logmsg("ERROR: truncate called on non-writable or non-regular file");
        return false;
    }
    else
    {
        if (m_iscontiguous)
        {
            revert_to_noncontiguous();
        }
        return m_fsfile.truncate(size);
    }
}

uint64_t ImageBackingStore::position()
{
#if ENABLE_COW
    // Handle Copy-on-Write mode
    if (m_iscow)
    {
        return m_cow.position();
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

#ifdef CONTAINER_IMAGE_SUPPORT
bool ImageBackingStore::isContainer()
{
    return m_fsfile.getContainerFormat() != ZuluContainerFs::Container::None;
}

const char *ImageBackingStore::containerTypeName()
{
    return m_fsfile.getContainerNameCstr();
}
#endif