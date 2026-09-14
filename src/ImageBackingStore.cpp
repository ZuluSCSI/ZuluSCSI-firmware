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

ImageBackingStore::ImageBackingStore()
{
    m_iscontiguous = false;
    m_israw = false;
    m_isrom = false;
    m_isreadonly_attr = false;
    m_blockdev = nullptr;
    m_bgnsector = m_endsector = m_cursector = 0;
    m_isfolder = false;
    m_foldername[0] = '\0';

    m_blockSize = 0;
    m_alignMode = ALIGN_UNALIGNED_OFF;
    m_logicalSector = 0;
    m_logicalSectorCount = 0;

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

    // Read AlignUnalignedAccesses up front, before the RAW: block's own
    // block-size check below -- that check predates this setting and
    // would otherwise unconditionally reject any AS/400 block size, since
    // 520/522 never divides 512 evenly. An explicit cisc/ppc request
    // always takes effect directly, regardless of scsi_block_size --
    // there are exactly two AS/400 block sizes in existence (520/522),
    // stable for decades, so there is no real incompatible combination to
    // guard against here; auto is what actually needs the block size to
    // pick a scheme.
    m_blockSize = scsi_block_size;
    m_alignMode = ALIGN_UNALIGNED_OFF;
    if (device_settings)
    {
        zuluscsi_align_unaligned_t configuredMode = (zuluscsi_align_unaligned_t)device_settings->alignUnalignedAccesses;
        m_alignMode = gapLayoutResolveAuto(configuredMode, scsi_block_size);

        if (configuredMode == ALIGN_UNALIGNED_AUTO)
        {
            logmsg("---- AlignUnalignedAccesses=auto resolved to ",
                   m_alignMode == ALIGN_UNALIGNED_CISC ? "cisc" : m_alignMode == ALIGN_UNALIGNED_PPC ? "ppc" : "off",
                   " for block size ", (int)scsi_block_size);
        }
    }

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

        if (m_alignMode == ALIGN_UNALIGNED_OFF && (scsi_block_size % SD_SECTOR_SIZE) != 0)
        {
            logmsg("SCSI block size ", (int)scsi_block_size, " is not supported for RAW partitions (must be divisible by 512 bytes, or use AlignUnalignedAccesses)");
            return;
        }

        m_iscontiguous = true;
        m_israw = true;
        m_blockdev = SD.card();

        uint32_t sectorCount = SD.card()->sectorCount();
        if (m_endsector >= sectorCount)
        {
            logmsg("---- Limiting RAW image mapping to SD card sector count: ", (int)sectorCount);
            m_endsector = sectorCount - 1;
        }

        setupGapLayout((uint64_t)(m_endsector - m_bgnsector + 1) * SD_SECTOR_SIZE);
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

    // Applies regardless of whether the fast raw path above engaged --
    // once AlignUnalignedAccesses is on, the file's own physical bytes are
    // gapped either way, so even the plain FsFile fallback path needs the
    // same logical/physical translation (see gappedTransfer()).
    setupGapLayout(m_fsfile.size());

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

void ImageBackingStore::setupGapLayout(uint64_t physicalSizeBytes)
{
    if (m_alignMode == ALIGN_UNALIGNED_OFF)
        return;

    m_logicalSector = 0;
    m_logicalSectorCount = gapLayoutLogicalSectorsInPhysicalSize(
        (zuluscsi_align_unaligned_t)m_alignMode, m_blockSize, physicalSizeBytes);
}

#ifdef PLATFORM_AS400
// Shared, single instance -- not per-ImageBackingStore-object. Safe because
// gappedTransfer() (the only caller) is only ever invoked from the main
// loop's own blocking SCSI command dispatch, never reentrantly (matches
// this codebase's established precedent for stack-tight buffers, e.g. the
// static conversions in custom_vendor_inquiry.cpp -- see JOURNAL.md).
//
// Batches several CISC slots/PPC groups per SD transaction instead of one
// at a time -- physical media has no real "gaps" (units sit back-to-back),
// so a multi-sector request is one genuinely contiguous physical run and
// can be handled as one transaction, not one per sector/group.
//
// Sizing: a real PlatformIO build (this session) measured free RAM at
// exactly 20768 bytes on ZuluSCSI_Blaster and 38280 bytes on ZuluSCSI_Wide
// (RAM: 96.0% / 92.7% of 524288 bytes total) -- ruling out the original
// idea of one buffer sized to the largest possible CDB (~126KB worst case
// under CISC), which would exceed Blaster's *entire* free RAM by roughly
// 6x. 8KB is a deliberately conservative fraction of that real headroom
// (specific values not yet re-measured after this session's own other
// additions -- IOTrace, TRIM, this feature's own code -- so free RAM
// today is likely less than either figure above), while still batching
// meaningfully more than one unit at a time: 8 CISC slots (8*1024=8192
// bytes exactly) or 1 PPC group (4608 of the 8192 bytes; a second group
// would need 9216, which doesn't fit) per transaction, versus 1 either
// way in the original single-unit implementation.
#define GAP_TRANSFER_BUFFER_SIZE 8192
static uint8_t s_gapStagingBuffer[GAP_TRANSFER_BUFFER_SIZE];
#endif

bool ImageBackingStore::gapUnitTransfer(uint64_t physOffset, uint32_t physSize, uint8_t *stagingBuf, bool isWrite)
{
    // IOTrace Layer B/DMA_WAIT: previously a deliberate gap (see JOURNAL.md)
    // -- gappedTransfer()/gapUnitTransfer() bypassed both entirely, so any
    // AlignUnalignedAccesses device's SD-access latency, sequentiality and
    // fast-path status were invisible to a capture, unlike every other
    // image type. sector/sectorCount are physical SD sectors (matches the
    // non-gapped branches' own m_cursector-based meaning below), not
    // logical AS/400 sectors -- physOffset/physSize are always exact
    // multiples of SD_SECTOR_SIZE by construction (see the comment on the
    // raw blockdev branch), so this division is always exact.
    uint32_t iotrace_sector = (uint32_t)(physOffset / SD_SECTOR_SIZE);
    uint16_t iotrace_sectorCount = (uint16_t)(physSize / SD_SECTOR_SIZE);
    uint8_t iotrace_flags = (isWrite ? IOTRACE_SD_FLAG_WRITE : 0) |
        (m_iscontiguous ? IOTRACE_SD_FLAG_CONTIGUOUS : 0);
    uint64_t iotrace_t0 = iotrace_now_us();
    bool ok;

    if (m_iscontiguous && m_blockdev)
    {
        // physOffset/physSize are always exact multiples of SD_SECTOR_SIZE
        // by construction (unit sizes GAP_CISC_SLOT_SIZE/GAP_PPC_GROUP_PHYS_SIZE
        // are both whole numbers of 512-byte sectors, and a multi-unit span
        // is just a sum of those) -- readSectors()/writeSectors() need
        // whole SD-sector-granular addressing, unlike the FsFile branch
        // below. Both already internally split large requests into
        // whatever chunk size the lower-level driver needs, so handing
        // them the whole span in one call (rather than unit-by-unit) is
        // safe regardless of how large physSize gets.
        uint32_t sdSector = m_bgnsector + (uint32_t)(physOffset / SD_SECTOR_SIZE);
        uint32_t sdSectorCount = physSize / SD_SECTOR_SIZE;
        ok = isWrite ? m_blockdev->writeSectors(sdSector, stagingBuf, sdSectorCount)
                     : m_blockdev->readSectors(sdSector, stagingBuf, sdSectorCount);
    }
    else if (m_fsfile.isOpen())
    {
        if (!m_fsfile.seek(physOffset))
            ok = false;
        else if (isWrite)
            ok = m_fsfile.write(stagingBuf, physSize) == (ssize_t)physSize;
        else
            ok = m_fsfile.read(stagingBuf, physSize) == (ssize_t)physSize;
    }
    else
    {
        ok = false;
    }

    uint32_t iotrace_duration_us = (uint32_t)(iotrace_now_us() - iotrace_t0);
    iotrace_loop_account(IOTRACE_BUCKET_DMA_WAIT, iotrace_duration_us);
    iotrace_sd_access(m_iotraceScsiId, iotrace_sector, iotrace_sectorCount, iotrace_flags, iotrace_duration_us);

    return ok;
}

ssize_t ImageBackingStore::gappedTransfer(void *buf, size_t count, bool isWrite)
{
#ifdef PLATFORM_AS400
    if (m_blockSize == 0 || (count % m_blockSize) != 0)
    {
        logmsg("ERROR: gapped image access count (", (int)count, ") is not a multiple of blockSize (", (int)m_blockSize, ")");
        return -1;
    }

    uint32_t sectorsWanted = (uint32_t)(count / m_blockSize);
    if (sectorsWanted == 0)
        return 0;

    zuluscsi_align_unaligned_t mode = (zuluscsi_align_unaligned_t)m_alignMode;
    uint32_t firstSector = m_logicalSector;
    uint8_t *cbuf = (uint8_t*)buf;
    uint32_t done = 0;

    // A request can span more sectors than one GAP_TRANSFER_BUFFER_SIZE-
    // bounded staging buffer can hold (that cap is deliberately much
    // smaller than the largest possible CDB -- see the buffer's own
    // declaration comment), so this processes it as a sequence of
    // buffer-bounded chunks, each still one contiguous SD transaction
    // covering as many whole units (CISC slots/PPC groups) as fit.
    while (done < sectorsWanted)
    {
        uint32_t chunkFirstSector = firstSector + done;
        uint32_t chunkRemaining = sectorsWanted - done;

        gap_layout_run_t startUnit;
        gapLayoutNextRun(mode, m_blockSize, chunkFirstSector, 1, &startUnit);
        uint64_t chunkPhysStart = startUnit.unitPhysicalOffset;

        // Greedily include whole units into this chunk until the next one
        // would overflow the staging buffer. GAP_TRANSFER_BUFFER_SIZE is
        // always larger than one whole unit (both schemes), so this
        // always makes forward progress -- at least the starting sector's
        // own unit is always included.
        uint32_t chunkSectors = 0;
        uint64_t chunkPhysEnd = chunkPhysStart;
        gap_layout_run_t lastUnitInChunk = startUnit;
        while (chunkSectors < chunkRemaining)
        {
            gap_layout_run_t sec;
            gapLayoutNextRun(mode, m_blockSize, chunkFirstSector + chunkSectors, 1, &sec);
            uint64_t secUnitEnd = sec.unitPhysicalOffset + sec.unitPhysicalSize;
            if (secUnitEnd - chunkPhysStart > GAP_TRANSFER_BUFFER_SIZE)
                break;
            chunkPhysEnd = secUnitEnd;
            lastUnitInChunk = sec;
            chunkSectors++;
        }

        uint32_t chunkBytes = (uint32_t)(chunkPhysEnd - chunkPhysStart);

        // Only the two edge units of this chunk can possibly be shared
        // with logical sectors outside the chunk's own range (true for
        // PPC groups; never for CISC, where every unit holds exactly one
        // sector, so these are always both "fully covered" there). Any
        // unit strictly between the edges is, by construction, entirely
        // covered by this chunk.
        bool firstUnitFullyCovered = (startUnit.sectorIndexInUnit == 0);
        bool lastUnitFullyCovered = (lastUnitInChunk.sectorIndexInUnit + 1 == lastUnitInChunk.unitSectorCapacity);

        if (isWrite)
        {
            if (!firstUnitFullyCovered || !lastUnitFullyCovered)
            {
                // An edge unit is only partially covered -- read this
                // chunk's span first so whatever it doesn't touch (other
                // sectors sharing that edge unit) survives being written
                // back. Harmless (if slightly wasteful) for any
                // fully-covered units caught up in the same read -- their
                // content gets fully overwritten below anyway.
                if (!gapUnitTransfer(chunkPhysStart, chunkBytes, s_gapStagingBuffer, false))
                    return done > 0 ? (ssize_t)((uint64_t)done * m_blockSize) : -1;
            }
            else
            {
                // Every unit in this chunk is being written in full from
                // its own start -- nothing to preserve anywhere, so just
                // blast fresh (zeroed) padding instead of reading first.
                memset(s_gapStagingBuffer, 0, chunkBytes);
            }

            for (uint32_t i = 0; i < chunkSectors; i++)
            {
                gap_layout_run_t sec;
                gapLayoutNextRun(mode, m_blockSize, chunkFirstSector + i, 1, &sec);
                uint64_t bufOffset = sec.unitPhysicalOffset + sec.byteOffsetInUnit - chunkPhysStart;
                memcpy(s_gapStagingBuffer + bufOffset, cbuf + (uint64_t)(done + i) * m_blockSize, m_blockSize);
            }

            if (!gapUnitTransfer(chunkPhysStart, chunkBytes, s_gapStagingBuffer, true))
                return done > 0 ? (ssize_t)((uint64_t)done * m_blockSize) : -1;
        }
        else
        {
            if (!gapUnitTransfer(chunkPhysStart, chunkBytes, s_gapStagingBuffer, false))
                return done > 0 ? (ssize_t)((uint64_t)done * m_blockSize) : -1;

            for (uint32_t i = 0; i < chunkSectors; i++)
            {
                gap_layout_run_t sec;
                gapLayoutNextRun(mode, m_blockSize, chunkFirstSector + i, 1, &sec);
                uint64_t bufOffset = sec.unitPhysicalOffset + sec.byteOffsetInUnit - chunkPhysStart;
                memcpy(cbuf + (uint64_t)(done + i) * m_blockSize, s_gapStagingBuffer + bufOffset, m_blockSize);
            }
        }

        done += chunkSectors;
    }

    m_logicalSector += sectorsWanted;
    return count;
#else
    // AlignUnalignedAccesses can only ever be turned on for PLATFORM_AS400
    // builds -- the ini parser only accepts the setting there (see
    // ZuluSCSI_settings.cpp) -- so m_alignMode can never be non-off here,
    // and this should be unreachable. Kept as a safe, explicit fallback
    // rather than assuming that invariant silently.
    logmsg("ERROR: gapped image access attempted on a build without AS/400 support");
    return -1;
#endif
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

bool ImageBackingStore::clampLogicalSectorCount(uint32_t maxSectors)
{
    if (m_alignMode == ALIGN_UNALIGNED_OFF)
        return false;
    if (maxSectors >= m_logicalSectorCount)
        return false;

    m_logicalSectorCount = maxSectors;
    return true;
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

    // Gapped images report their *logical* size here, not the larger
    // physical/on-SD-card size below -- this is what ZuluSCSI_disk.cpp
    // divides by blockSize to get the SCSI-reported sector count
    // (img.scsiSectors = img.file.size() / blocksize), so returning the
    // physical size here would misreport capacity to the host.
    if (m_alignMode != ALIGN_UNALIGNED_OFF)
    {
        return (uint64_t)m_logicalSectorCount * m_blockSize;
    }

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

    if (m_alignMode != ALIGN_UNALIGNED_OFF)
    {
        // Gapped layout: pos is a *logical* byte offset (sector*blockSize)
        // -- entirely separate from the raw-SD-sector-aligned math below,
        // which only applies when AlignUnalignedAccesses is off. A CDB
        // should never seek to a non-sector-aligned logical position;
        // refuse rather than guess if one somehow does.
        if (m_blockSize == 0 || (pos % m_blockSize) != 0)
        {
            dbgmsg("---- Unaligned access to gapped image (pos not a multiple of blockSize)");
            return false;
        }

        m_logicalSector = (uint32_t)(pos / m_blockSize);
        return m_logicalSector < m_logicalSectorCount;
    }

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

    if (m_alignMode != ALIGN_UNALIGNED_OFF)
    {
        // IOTrace instrumentation lives inside gapUnitTransfer() itself
        // (Layer B/DMA_WAIT), not here -- see its own comment.
        return gappedTransfer(buf, count, false);
    }

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

    if (m_alignMode != ALIGN_UNALIGNED_OFF)
    {
        if (m_isreadonly_attr)
        {
            logmsg("ERROR: attempted to write to a read only image");
            return 0;
        }
        // IOTrace instrumentation lives inside gapUnitTransfer() itself --
        // see the matching comment in read() above.
        return gappedTransfer((void*)buf, count, true);
    }

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

    // Skip Write/Read (ZuluSCSI_disk.cpp's scsiDiskSkip() machinery) walks
    // its sector mask via img.file.seek(img.file.position() + skip_bytes),
    // and depends on this returning a real, current logical position for
    // that to seek correctly -- this matters a great deal here, since
    // AlignUnalignedAccesses=ppc's main real-world motivation *is* making
    // the fast path safe for exactly the PPC images Skip Write already
    // runs against. (Unlike the non-gapped fast path just below, which
    // returns 0 regardless of actual position -- a pre-existing,
    // documented limitation this doesn't change.)
    if (m_alignMode != ALIGN_UNALIGNED_OFF)
    {
        return (uint64_t)m_logicalSector * m_blockSize;
    }

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