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
#include "ZuluSCSI_gap_layout.h"
#ifdef CONTAINER_IMAGE_SUPPORT
#include <ZCFsFile.h>
#endif
extern "C" {
#include <scsi.h>
}

#if ENABLE_COW
#include "COWStorage.h"
#endif

// SD card sector size is always 512 bytes
extern SdFs SD;
#define SD_SECTOR_SIZE 512

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


    // For virtual devices, currently network only devices
    ImageBackingStore(bool open);

    // Parse image file parameters from filename.
    // Special filename formats:
    //    RAW:start:end
    //    ROM:
    //    *.cow (enables copy-on-write)
    // scsiId is for IOTrace Layer B/image-open logging only (see setScsiId()
    // below) -- passed here, rather than set afterwards via setScsiId(),
    // because _internal_open() runs during construction and needs it
    // already known at that point. Callers that don't reconstruct the
    // object (i.e. don't need _internal_open() to run again) can keep using
    // setScsiId() instead.
    ImageBackingStore(const char *filename, uint32_t scsi_block_size, scsi_device_settings_t *device_config, uint8_t scsiId = 0xFF);



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

    // Which SCSI ID this image is currently bound to, for IOTrace Layer B
    // and image-open logging only (see ZuluSCSI_iotrace.h) -- ImageBackingStore
    // itself has no other use for this and doesn't otherwise know its own
    // target. Set by the caller (ZuluSCSI_disk.cpp) alongside image_config_t's
    // own S2S_TargetCfg::scsiId. Defaults to 0xFF (unknown) until set.
    // Prefer passing scsiId to the constructor instead when (re)constructing
    // an image -- see its comment above.
    void setScsiId(uint8_t scsiId) { m_iotraceScsiId = scsiId; }

    // Close the image so that .isOpen() will return false.
    bool close();

    // Return image size in bytes
    uint64_t size();

    // Gapped mode only: shrink the logical sector count down to at most
    // maxSectors, so size() (and therefore ReadCapacity and the read/write
    // bounds check, both of which derive capacity from size()) report a
    // caller-supplied true capacity instead of "however many whole gapped
    // units physically fit in the backing store" -- needed when a PART:n
    // partition has margin beyond what an AS400_DiskProfile actually
    // declares. No-op (returns false) outside gapped mode, or if
    // maxSectors isn't smaller than the current count.
    bool clampLogicalSectorCount(uint32_t maxSectors);

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

    // Truncate the file to the specified size.
    bool truncate(uint64_t size);

    size_t getFilename(char* buf, size_t buflen);

    // Change image if the image is a folder (used for .cue with multiple .bin)
    bool selectImageFile(const char *filename);
    size_t getFoldername(char* buf, size_t buflen);
#ifdef CONTAINER_IMAGE_SUPPORT
    // Return true if the image is contained in a container file like vhd
    bool isContainer();
    // Return the name of the container type
    const char *containerTypeName();
#endif

protected:
    bool m_iscontiguous;
    bool m_israw;
    bool m_isrom;
    bool m_isproccesor;
    bool m_isprocessor_open;
    bool m_isreadonly_attr;
    romdrive_hdr_t m_romhdr;
#ifdef CONTAINER_IMAGE_SUPPORT
    ZuluContainerFs::ZCFsFile m_fsfile;
#else
    FsFile m_fsfile;
#endif
    SdCard *m_blockdev;
    uint32_t m_bgnsector;
    uint32_t m_endsector;
    uint32_t m_cursector;
    uint8_t m_iotraceScsiId = 0xFF;

    // AlignUnalignedAccesses support -- see ZuluSCSI_gap_layout.h for the
    // physical layout this implements. m_blockSize is the *logical* AS/400
    // sector size (e.g. 520/522), independent of SD_SECTOR_SIZE. m_alignMode
    // is ALIGN_UNALIGNED_OFF for every non-AS/400 image (the overwhelming
    // majority) -- seek()/read()/write() branch to the gapped-layout code
    // path only when it isn't, leaving the existing, long-relied-on
    // non-gapped path completely untouched otherwise.
    uint32_t m_blockSize;
    uint8_t m_alignMode;
    // Current position in *logical* sectors, used only when m_alignMode !=
    // ALIGN_UNALIGNED_OFF -- a separate concept from m_cursector above,
    // which (for the non-gapped path) counts physical 512-byte SD sectors.
    uint32_t m_logicalSector;
    // Total logical sectors available, computed once at open time from the
    // backing store's actual physical size -- see
    // gapLayoutLogicalSectorsInPhysicalSize(). Bounds seek() the same way
    // m_endsector bounds the non-gapped raw path.
    uint32_t m_logicalSectorCount;

    bool m_isfolder;
    char m_foldername[MAX_FILE_PATH + 1];

    bool _internal_open(const char *filename);

    void revert_to_noncontiguous();

    // Sets up m_blockSize/m_alignMode/m_logicalSectorCount from the
    // constructor's scsi_block_size/device_settings. Called once, near
    // the end of construction, after m_bgnsector/m_endsector or m_fsfile
    // are already set up -- physicalSizeBytes is whatever backing store
    // the caller already determined (raw extent size, or the opened
    // file's size).
    void setupGapLayout(uint64_t physicalSizeBytes);

    // Gapped-layout read/write, used in place of the normal dispatch in
    // read()/write() whenever m_alignMode != ALIGN_UNALIGNED_OFF. Shared
    // between the raw blockdev backend (RAW:/PART:, or a plain file
    // promoted to the contiguous fast path) and the FsFile backend (a
    // plain, possibly-fragmented file) -- both are addressed identically
    // in terms of gapLayoutNextRun()'s logical/physical translation, only
    // the actual low-level transfer primitive (gapUnitTransfer() below)
    // differs between them. Handles the whole request's contiguous
    // physical span in one shot (one SD transaction per call, not one per
    // CISC slot/PPC group) via a shared staging buffer sized to the
    // worst-case span a single call can ever need -- see that buffer's
    // own declaration comment in the .cpp for the exact derivation.
    ssize_t gappedTransfer(void *buf, size_t count, bool isWrite);

    // Performs one contiguous physical transfer spanning [physOffset,
    // physOffset+physSize) -- may cover several CISC slots/PPC groups at
    // once, not just one -- dispatching to the raw blockdev (whole SD
    // sectors) or m_fsfile (seek + read/write) depending on which backend
    // this instance uses.
    bool gapUnitTransfer(uint64_t physOffset, uint32_t physSize, uint8_t *stagingBuf, bool isWrite);

#if ENABLE_COW
    bool m_iscow;
    COWStorage m_cow;
#endif
};
