/* Tape device emulation
 * Will be called by scsi.c from SCSI2SD.
 *
 * ZuluSCSI™ - Copyright (c) 2023-2025 Rabbit Hole Computing™
 * Copyright (c) 2023 Kars de Jong
 * SimH magtape support
 * Copyright (C) 2025 by Stefan Reinauer
 * https://simh.trailing-edge.com/docs/simh_magtape.pdf
 *
 * This file is licensed under the GPL version 3 or any later version. 
 * It is derived from cdrom.c in SCSI2SD V6
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
 */

#include "ZuluSCSI_disk.h"
#include "ZuluSCSI_log.h"
#include "ZuluSCSI_config.h"
#include "ZuluSCSI_tape.h"
#include <ZuluSCSI_platform.h>
#include <scsiPhy.h>

extern "C" {
#include <scsi.h>
}

// scsiStartRead is defined in ZuluSCSI_disk.cpp for platforms without non-blocking read
extern void scsiStartRead(uint8_t* data, uint32_t count, int *parityError);

#ifdef PREFETCH_BUFFER_SIZE

#endif

// Helper function to read little-endian 32-bit value
static uint32_t readLE32(const uint8_t *data) {
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

// Helper function to write little-endian 32-bit value
static void writeLE32(uint8_t *data, uint32_t value) {
    data[0] = value & 0xFF;
    data[1] = (value >> 8) & 0xFF;
    data[2] = (value >> 16) & 0xFF;
    data[3] = (value >> 24) & 0xFF;
}

// Read a .TAP record moving forward
tap_result_t tapReadRecordForward(image_config_t &img, tap_record_t &record, uint8_t *buffer, uint32_t buffer_size) {
    record.is_error = false;
    record.is_filemark = false;
    record.is_eom = false;

    uint8_t header[4];

    // Check if we're at end of file
    if (img.tape_pos >= img.file.size()) {
        record.is_eom = true;
        return TAP_END_OF_TAPE;
    }

    // Read 4-byte header
    if (!img.file.seek(img.tape_pos) || img.file.read(header, 4) != 4) {
        record.is_error = true;
        return TAP_ERROR;
    }

    uint32_t length_with_class = readLE32(header);
    record.record_class = (length_with_class >> 28) & 0xF;
    record.length = length_with_class & 0x0FFFFFFF;

    // Check for special markers
    if (length_with_class == TAP_MARKER_TAPEMARK) {
        record.is_filemark = true;
        img.tape_pos += 4;
        return TAP_FILEMARK;
    }

    if (length_with_class == TAP_MARKER_END_MEDIUM) {
        record.is_eom = true;
        return TAP_END_OF_TAPE;
    }

    // For data records, read the data if buffer provided
    if (record.length > 0) {
        uint32_t data_length = record.length;
        uint32_t padded_length = (data_length + 1) & ~1;  // Round up to even

        if (buffer && buffer_size >= data_length) {
            if (!img.file.seek(img.tape_pos + 4) || img.file.read(buffer, data_length) != (ssize_t)data_length) {
                record.is_error = true;
                return TAP_ERROR;
            }
        }

        // Verify trailing length
        uint8_t trailer[4];
        if (!img.file.seek(img.tape_pos + 4 + padded_length) || img.file.read(trailer, 4) != 4) {
            record.is_error = true;
            return TAP_ERROR;
        }

        uint32_t trailing_length = readLE32(trailer) & 0x0FFFFFFF;
        if (trailing_length != record.length) {
            dbgmsg("------ TAP record length mismatch: header=", (int)record.length, " trailer=", (int)trailing_length);
            record.is_error = true;
            return TAP_ERROR;
        }

        // Move past this record
        img.tape_pos += 8 + padded_length;
    } else {
        // Zero-length record, move past header
        img.tape_pos += 4;
    }

    return TAP_OK;
}

// Read a .TAP record moving backward
tap_result_t tapReadRecordBackward(image_config_t &img, tap_record_t &record, uint8_t *buffer, uint32_t buffer_size) {
    record.is_error = false;
    record.is_filemark = false;
    record.is_eom = false;

    // Check if we're at beginning of tape
    if (img.tape_pos == 0) {
        return TAP_BEGINNING_OF_TAPE;
    }

    uint8_t trailer[4];

    // Read 4 bytes before current position (trailing length)
    if (img.tape_pos < 4 || !img.file.seek(img.tape_pos - 4) || img.file.read(trailer, 4) != 4) {
        record.is_error = true;
        return TAP_ERROR;
    }

    uint32_t length_with_class = readLE32(trailer);

    // Check for special markers
    if (length_with_class == TAP_MARKER_TAPEMARK) {
        record.is_filemark = true;
        img.tape_pos -= 4;
        return TAP_FILEMARK;
    }

    if (length_with_class == TAP_MARKER_END_MEDIUM) {
        record.is_eom = true;
        img.tape_pos -= 4;
        return TAP_END_OF_TAPE;
    }

    record.record_class = (length_with_class >> 28) & 0xF;
    record.length = length_with_class & 0x0FFFFFFF;

    if (record.length > 0) {
        uint32_t padded_length = (record.length + 1) & ~1;
        uint32_t total_length = 8 + padded_length;

        if (img.tape_pos < total_length) {
            record.is_error = true;
            return TAP_ERROR;
        }

        // Move to start of record
        img.tape_pos -= total_length;

        // Verify header length matches
        uint8_t header[4];
        if (!img.file.seek(img.tape_pos) || img.file.read(header, 4) != 4) {
            record.is_error = true;
            return TAP_ERROR;
        }

        uint32_t header_length = readLE32(header) & 0x0FFFFFFF;
        if (header_length != record.length) {
            dbgmsg("------ TAP backward record length mismatch: header=", (int)header_length, " trailer=", (int)record.length);
            record.is_error = true;
            return TAP_ERROR;
        }

        // Read data if buffer provided
        if (buffer && buffer_size >= record.length) {
            if (!img.file.seek(img.tape_pos + 4) || img.file.read(buffer, record.length) != (ssize_t)record.length) {
                record.is_error = true;
                return TAP_ERROR;
            }
        }
    } else {
        // Zero-length record
        img.tape_pos -= 4;
    }

    return TAP_OK;
}

// Write a .TAP data record
tap_result_t tapWriteRecord(image_config_t &img, const uint8_t *data, uint32_t length) {
    uint32_t padded_length = (length + 1) & ~1;  // Round up to even
    uint8_t header[4], trailer[4];

    // Write header (length with class 0 for good data)
    writeLE32(header, length);
    if (!img.file.seek(img.tape_pos) || img.file.write(header, 4) != 4) {
        return TAP_ERROR;
    }

    // Write data
    if (length > 0) {
        if (!img.file.seek(img.tape_pos + 4) || img.file.write(data, length) != (ssize_t)length) {
            return TAP_ERROR;
        }

        // Write padding byte if needed
        if (padded_length > length) {
            uint8_t pad = 0;
            if (!img.file.seek(img.tape_pos + 4 + length) || img.file.write(&pad, 1) != 1) {
                return TAP_ERROR;
            }
        }
    }

    // Write trailer (same length)
    writeLE32(trailer, length);
    if (!img.file.seek(img.tape_pos + 4 + padded_length) || img.file.write(trailer, 4) != 4) {
        return TAP_ERROR;
    }

    // Move past this record
    img.tape_pos += 8 + padded_length;

    return TAP_OK;
}

// Write a filemark
tap_result_t tapWriteFilemark(image_config_t &img) {
    uint8_t marker[4];
    writeLE32(marker, TAP_MARKER_TAPEMARK);

    if (!img.file.seek(img.tape_pos) || img.file.write(marker, 4) != 4) {
        return TAP_ERROR;
    }

    img.tape_pos += 4;
    return TAP_OK;
}

// Write an end-of-medium mark
tap_result_t tapWriteEOM(image_config_t &img) {
    uint8_t marker[4];
    writeLE32(marker, TAP_MARKER_END_MEDIUM);

    if (!img.file.seek(img.tape_pos) || img.file.write(marker, 4) != 4) {
        return TAP_ERROR;
    }

    img.tape_pos += 4;
    return TAP_OK;
}

// Write an erase gap
tap_result_t tapWriteEraseGap(image_config_t &img) {
    uint8_t marker[4];
    writeLE32(marker, TAP_MARKER_ERASE_GAP);

    if (!img.file.seek(img.tape_pos) || img.file.write(marker, 4) != 4) {
        return TAP_ERROR;
    }

    img.tape_pos += 4;
    return TAP_OK;
}

// Space forward by records or filemarks
tap_result_t tapSpaceForward(image_config_t &img, uint32_t count, bool filemarks) {
    tap_record_t record;
    uint8_t dummy_buffer[1];  // Small buffer for record parsing

    for (uint32_t i = 0; i < count; i++) {
        tap_result_t result = tapReadRecordForward(img, record, dummy_buffer, sizeof(dummy_buffer));

        if (result == TAP_END_OF_TAPE) {
            return TAP_END_OF_TAPE;
        }

        if (result == TAP_ERROR) {
            return TAP_ERROR;
        }

        if (filemarks) {
            // Spacing filemarks - stop when we hit one
            if (result == TAP_FILEMARK) {
                break;
            }
        } else {
            // Spacing records - filemarks are also records for spacing purposes
            // Continue until count reached or end of tape
        }
    }

    return TAP_OK;
}

// Space backward by records or filemarks
tap_result_t tapSpaceBackward(image_config_t &img, uint32_t count, bool filemarks) {
    tap_record_t record;
    uint8_t dummy_buffer[1];

    for (uint32_t i = 0; i < count; i++) {
        tap_result_t result = tapReadRecordBackward(img, record, dummy_buffer, sizeof(dummy_buffer));

        if (result == TAP_BEGINNING_OF_TAPE) {
            return TAP_BEGINNING_OF_TAPE;
        }

        if (result == TAP_ERROR) {
            return TAP_ERROR;
        }

        if (filemarks) {
            // Spacing filemarks - stop when we hit one
            if (result == TAP_FILEMARK) {
                break;
            }
        }
    }

    return TAP_OK;
}

// Rewind to beginning of tape
tap_result_t tapRewind(image_config_t &img) {
    img.tape_pos = 0;
    return TAP_OK;
}

// State tracking for .TAP format writes
struct tap_transfer_t {
    uint32_t record_length;     // Total length of record being written
    uint32_t bytes_written;     // Bytes written so far
    bool is_fixed_mode;         // Fixed vs variable block mode
    bool header_written;        // Has 4-byte header been written?
    uint32_t file_pos_start;    // File position at start of record

    // Fields for data transfer management (similar to g_disk_transfer)
    uint8_t* buffer;            // Buffer for data transfer
    uint32_t bytes_scsi;        // Total SCSI buffer size
    uint32_t bytes_sd;          // Bytes processed so far
    uint32_t bytes_scsi_started; // Bytes started in SCSI transfer
    uint32_t sd_transfer_start; // Start position for SD transfer
    int parityError;            // Parity error flag
};

static tap_transfer_t g_tap_transfer;

#ifdef PREFETCH_BUFFER_SIZE
static struct {
    uint8_t buffer[PREFETCH_BUFFER_SIZE];
    uint32_t position;          // Tape position for prefetch data
    uint32_t bytes;             // Bytes available in prefetch buffer
    uint8_t scsiId;             // SCSI ID for this prefetch
} g_tape_prefetch;
#endif

// Check if the tape file is in .TAP format
bool tapIsTapFormat(image_config_t &img) {
    // Check file extension first
    char filename[MAX_FILE_PATH];
    img.file.getFilename(filename, sizeof(filename));
    char *ext = strrchr(filename, '.');
    if (ext && (strcasecmp(ext, ".tap") == 0 || strcasecmp(ext, ".TAP") == 0)) {
        return true;
    }

    // Could also check file content here if needed
    // For now, rely on .tap extension
    return false;
}

// .TAP-aware read function
static void doTapRead(image_config_t &img, uint32_t length, bool fixed) {
    tap_record_t record;
    uint8_t *buffer = scsiDev.data;
    uint32_t buffer_size = sizeof(scsiDev.data);

    if (fixed) {
        // Fixed block mode - read exactly one record, fail if length doesn't match
        tap_result_t result = tapReadRecordForward(img, record, buffer, buffer_size);

        if (result == TAP_FILEMARK) {
            scsiDev.target->sense.filemark = true;
            scsiDev.status = CHECK_CONDITION;
            scsiDev.target->sense.code = NO_SENSE;
            scsiDev.target->sense.asc = NO_ADDITIONAL_SENSE_INFORMATION;
            scsiDev.phase = STATUS;
            return;
        } else if (result == TAP_END_OF_TAPE) {
            scsiDev.target->sense.eom = true;
            scsiDev.status = CHECK_CONDITION;
            scsiDev.target->sense.code = BLANK_CHECK;
            scsiDev.target->sense.asc = NO_ADDITIONAL_SENSE_INFORMATION;
            scsiDev.phase = STATUS;
            return;
        } else if (result == TAP_ERROR) {
            scsiDev.status = CHECK_CONDITION;
            scsiDev.target->sense.code = MEDIUM_ERROR;
            scsiDev.target->sense.asc = NO_ADDITIONAL_SENSE_INFORMATION;
            scsiDev.phase = STATUS;
            return;
        }

        // Check if record length matches requested length
        if (record.length != length) {
            dbgmsg("------ TAP fixed block length mismatch: requested=", (int)length, " actual=", (int)record.length);
            scsiDev.status = CHECK_CONDITION;
            scsiDev.target->sense.code = ILLEGAL_REQUEST;
            scsiDev.target->sense.asc = INVALID_FIELD_IN_CDB;
            scsiDev.phase = STATUS;
            return;
        }

        scsiDev.dataLen = record.length;
        scsiDev.phase = DATA_IN;
    } else {
        // Variable block mode - read one record, return its length
        tap_result_t result = tapReadRecordForward(img, record, buffer,
                                                   length < buffer_size ? length : buffer_size);

        if (result == TAP_FILEMARK) {
            scsiDev.target->sense.filemark = true;
            scsiDev.status = CHECK_CONDITION;
            scsiDev.target->sense.code = NO_SENSE;
            scsiDev.target->sense.asc = NO_ADDITIONAL_SENSE_INFORMATION;
            scsiDev.phase = STATUS;
            return;
        } else if (result == TAP_END_OF_TAPE) {
            scsiDev.target->sense.eom = true;
            scsiDev.status = CHECK_CONDITION;
            scsiDev.target->sense.code = BLANK_CHECK;
            scsiDev.target->sense.asc = NO_ADDITIONAL_SENSE_INFORMATION;
            scsiDev.phase = STATUS;
            return;
        } else if (result == TAP_ERROR) {
            scsiDev.status = CHECK_CONDITION;
            scsiDev.target->sense.code = MEDIUM_ERROR;
            scsiDev.target->sense.asc = NO_ADDITIONAL_SENSE_INFORMATION;
            scsiDev.phase = STATUS;
            return;
        }

        // Check if record is larger than requested buffer
        if (record.length > length) {
            dbgmsg("------ TAP variable block too large: record=", (int)record.length, " buffer=", (int)length);
            scsiDev.status = CHECK_CONDITION;
            scsiDev.target->sense.code = ILLEGAL_REQUEST;
            scsiDev.target->sense.asc = INVALID_FIELD_IN_CDB;
            scsiDev.phase = STATUS;
            return;
        }

        scsiDev.dataLen = record.length;
        scsiDev.phase = DATA_IN;
    }
}

// Start a .TAP format write operation
void tapeMediumStartWrite(uint32_t length, bool fixed) {
    image_config_t &img = *(image_config_t*)scsiDev.target->cfg;

    dbgmsg("------ TAP write ", (int)length, " bytes, fixed=", fixed ? 1 : 0);

    // Check write protection
    if (unlikely(!img.file.isWritable())) {
        logmsg("WARNING: Host attempted write to read-only .TAP file");
        scsiDev.status = CHECK_CONDITION;
        scsiDev.target->sense.code = ILLEGAL_REQUEST;
        scsiDev.target->sense.asc = WRITE_PROTECTED;
        scsiDev.phase = STATUS;
        return;
    }

    // Initialize .TAP transfer state
    g_tap_transfer.record_length = length;
    g_tap_transfer.bytes_written = 0;
    g_tap_transfer.is_fixed_mode = fixed;
    g_tap_transfer.header_written = false;
    g_tap_transfer.file_pos_start = img.tape_pos;

    // Set up SCSI transfer similar to scsiDiskStartWrite
    transfer.multiBlock = true;
    transfer.lba = 0;  // Not used for .TAP
    transfer.blocks = 1;  // We handle one record at a time
    transfer.currentBlock = 0;

    scsiDev.phase = DATA_OUT;
    scsiDev.dataLen = 0;
    scsiDev.dataPtr = 0;
    scsiDev.postDataOutHook = tapeDataOut;

#ifdef PREFETCH_BUFFER_SIZE
    // Invalidate tape prefetch buffer
    g_tape_prefetch.bytes = 0;
    g_tape_prefetch.position = 0;
#endif
}

// .TAP-aware data output handler
void tapeDataOut() {
    scsiEnterPhase(DATA_OUT);

    image_config_t &img = *(image_config_t*)scsiDev.target->cfg;
    uint32_t bufsize = sizeof(scsiDev.data);

    // Initialize tape transfer structure for data collection
    g_tap_transfer.buffer = scsiDev.data;
    g_tap_transfer.bytes_scsi = bufsize;  // Use full buffer capacity
    g_tap_transfer.bytes_sd = 0;
    g_tap_transfer.bytes_scsi_started = 0;
    g_tap_transfer.sd_transfer_start = 0;
    g_tap_transfer.parityError = 0;

    while (g_tap_transfer.bytes_sd < g_tap_transfer.bytes_scsi
           && scsiDev.phase == DATA_OUT
           && !scsiDev.resetFlag
           && g_tap_transfer.bytes_written < g_tap_transfer.record_length)
    {
        platform_poll();
        diskEjectButtonUpdate(false);

        // Calculate how much data we still need
        uint32_t remaining = g_tap_transfer.record_length - g_tap_transfer.bytes_written;
        uint32_t available = g_tap_transfer.bytes_scsi_started - g_tap_transfer.bytes_sd;

        // Don't write more than the record length
        if (available > remaining) {
            available = remaining;
        }

        // Write the .TAP record header if not done yet
        if (!g_tap_transfer.header_written && available > 0) {
            uint8_t header[4];
            writeLE32(header, g_tap_transfer.record_length);

            if (!img.file.seek(img.tape_pos) || img.file.write(header, 4) != 4) {
                scsiDev.status = CHECK_CONDITION;
                scsiDev.target->sense.code = MEDIUM_ERROR;
                scsiDev.target->sense.asc = NO_ADDITIONAL_SENSE_INFORMATION;
                scsiDev.phase = STATUS;
                return;
            }

            img.tape_pos += 4;
            g_tap_transfer.header_written = true;
            dbgmsg("------ TAP wrote record header, length=", (int)g_tap_transfer.record_length);
        }

        // Write available data to file
        if (available > 0) {
            uint32_t start = g_tap_transfer.bytes_sd % bufsize;
            uint32_t len = available;

            // Handle buffer wraparound
            if (start + len > bufsize) {
                len = bufsize - start;
            }

            if (!img.file.seek(img.tape_pos) || img.file.write(&scsiDev.data[start], len) != (ssize_t)len) {
                scsiDev.status = CHECK_CONDITION;
                scsiDev.target->sense.code = MEDIUM_ERROR;
                scsiDev.target->sense.asc = NO_ADDITIONAL_SENSE_INFORMATION;
                scsiDev.phase = STATUS;
                return;
            }

            img.tape_pos += len;
            g_tap_transfer.bytes_written += len;
            g_tap_transfer.bytes_sd += len;

            dbgmsg("------ TAP wrote ", (int)len, " bytes, total=", (int)g_tap_transfer.bytes_written);
        }

        // Read more data from SCSI bus if needed
        if (g_tap_transfer.bytes_scsi_started < bufsize &&
            g_tap_transfer.bytes_written < g_tap_transfer.record_length) {

            uint32_t needed = g_tap_transfer.record_length - g_tap_transfer.bytes_written;
            uint32_t to_read = (needed < bufsize) ? needed : bufsize;

            scsiStartRead(&scsiDev.data[g_tap_transfer.bytes_scsi_started],
                         to_read, &g_tap_transfer.parityError);
            g_tap_transfer.bytes_scsi_started += to_read;
        }
    }

    // Write record complete - add padding and trailing length
    if (g_tap_transfer.bytes_written >= g_tap_transfer.record_length) {
        // Add padding byte if record length is odd
        if (g_tap_transfer.record_length & 1) {
            uint8_t pad = 0;
            if (!img.file.seek(img.tape_pos) || img.file.write(&pad, 1) != 1) {
                scsiDev.status = CHECK_CONDITION;
                scsiDev.target->sense.code = MEDIUM_ERROR;
                scsiDev.target->sense.asc = NO_ADDITIONAL_SENSE_INFORMATION;
                scsiDev.phase = STATUS;
                return;
            }
            img.tape_pos += 1;
        }

        // Write trailing length
        uint8_t trailer[4];
        writeLE32(trailer, g_tap_transfer.record_length);

        if (!img.file.seek(img.tape_pos) || img.file.write(trailer, 4) != 4) {
            scsiDev.status = CHECK_CONDITION;
            scsiDev.target->sense.code = MEDIUM_ERROR;
            scsiDev.target->sense.asc = NO_ADDITIONAL_SENSE_INFORMATION;
            scsiDev.phase = STATUS;
            return;
        }

        img.tape_pos += 4;

        dbgmsg("------ TAP record complete, final position=", (int)img.tape_pos);
        scsiFinishWrite();

        // Mark transfer as complete
        transfer.currentBlock = transfer.blocks;
        scsiDev.status = GOOD;
        scsiDev.phase = STATUS;
    }
}

static void doSeek(uint32_t lba)
{
    image_config_t &img = *(image_config_t*)scsiDev.target->cfg;
    uint32_t bytesPerSector = scsiDev.target->liveCfg.bytesPerSector;
    uint32_t capacity = img.file.size() / bytesPerSector;

    dbgmsg("------ Locate tape to LBA ", (int)lba);

    if (lba >= capacity)
    {
        scsiDev.status = CHECK_CONDITION;
        scsiDev.target->sense.code = ILLEGAL_REQUEST;
        scsiDev.target->sense.asc = LOGICAL_BLOCK_ADDRESS_OUT_OF_RANGE;
        scsiDev.phase = STATUS;
    }
    else
    {
        delay(10);
        img.tape_pos = lba;

        scsiDev.status = GOOD;
        scsiDev.phase = STATUS;
    }
}

static void doTapeRead(uint32_t blocks)
{
    image_config_t &img = *(image_config_t*)scsiDev.target->cfg;

    if (img.tape_length_mb > 0)
    {
        uint32_t capacity_mb = img.tape_length_mb;
        uint64_t capacity_bytes = (uint64_t)capacity_mb * 1024 * 1024;
        uint32_t block_size = scsiDev.target->liveCfg.bytesPerSector;
        uint32_t capacity_blocks = capacity_bytes / block_size;

        if (img.tape_pos >= capacity_blocks)
        {
            scsiDev.target->sense.eom = true;
            scsiDev.status = CHECK_CONDITION;
            scsiDev.target->sense.code = BLANK_CHECK;
            scsiDev.target->sense.asc = NO_ADDITIONAL_SENSE_INFORMATION;
            scsiDev.phase = STATUS;
            return;
        }

        if (img.tape_pos + blocks > capacity_blocks)
        {
            blocks = capacity_blocks - img.tape_pos;
            scsiDev.target->sense.eom = true;
        }
    }

    uint32_t capacity_lba = img.get_capacity_lba();

    if (img.tape_load_next_file && img.bin_container.isOpen())
    {
        // multifile tape - multiple file markers
        char dir_name[MAX_FILE_PATH + 1];
        char current_filename[MAX_FILE_PATH + 1] = {0};
        char next_filename[MAX_FILE_PATH + 1] = {0};
        int filename_len = 0;
        img.bin_container.getName(dir_name, sizeof(dir_name));
        img.file.getFilename(current_filename, sizeof(current_filename));
        img.tape_load_next_file = false;
        // load first file in directory or load next file

        filename_len = findNextImageAfter(img, dir_name, current_filename, next_filename, sizeof(next_filename), true);
        if (filename_len > 0 && img.file.selectImageFile(next_filename))
        {
            if (img.tape_mark_index > 0)
            {
                img.tape_mark_block_offset += capacity_lba;
            }
            else
            {
                img.tape_mark_block_offset = 0;
            }
            capacity_lba = img.get_capacity_lba();

            dbgmsg("------ Read tape loaded file ", next_filename, " has ", (int) capacity_lba, " sectors with filemark ", (int) img.tape_mark_index ," at the end");
        }
        else
        {
            logmsg("No tape element images found or openable in tape directory ", dir_name);
            scsiDev.target->sense.filemark = true;
            scsiDev.status = CHECK_CONDITION;
            scsiDev.target->sense.code = MEDIUM_ERROR;
            scsiDev.target->sense.asc = MEDIUM_NOT_PRESENT;
            scsiDev.phase = STATUS;
            return;
        }
    }

    if (img.file.isOpen())
    {
        bool passed_filemarker = false;
        uint32_t blocks_till_eof = 0;
        if (unlikely(((uint64_t) img.tape_pos) - img.tape_mark_block_offset + blocks >= capacity_lba))
        {
            // reading past a file, set blocks to end of file
            blocks_till_eof =  capacity_lba - (img.tape_pos - img.tape_mark_block_offset);
            passed_filemarker = true;
            // SCSI-2 Spec: "If the fixed bit is one, the information field shall be set to the requested transfer length minus the
            //               actual number of blocks read (not including the filemark)"
            scsiDev.target->sense.info = blocks - blocks_till_eof;
            dbgmsg("------ Read tape went past file marker, blocks left to be read ", (int) blocks_till_eof, " out of ", (int) blocks, " sense info set to ", (int) scsiDev.target->sense.info);

            blocks = blocks_till_eof;
        }

        if (blocks > 0)
        {
            dbgmsg("------ Read tape ", (int)blocks, "x", (int)scsiDev.target->liveCfg.bytesPerSector, " tape position ",(int)img.tape_pos,
                            " file position ", (int)(img.tape_pos - img.tape_mark_block_offset), " ends with file mark ",
                            (int)(img.tape_mark_index + 1), "/", (int) img.tape_mark_count, passed_filemarker ? " reached" : " not reached");
            scsiDiskStartRead(img.tape_pos - img.tape_mark_block_offset, blocks);
            scsiFinishWrite();
            img.tape_pos += blocks;
        }

        if (passed_filemarker)
        {
            if (img.tape_mark_index < img.tape_mark_count)
            {
                img.tape_mark_index++;
                if(img.tape_mark_index < img.tape_mark_count)
                    img.tape_load_next_file = true;

                scsiDev.target->sense.filemark = true;
                scsiDev.status = CHECK_CONDITION;
                scsiDev.target->sense.code = NO_SENSE;
                scsiDev.target->sense.asc = NO_ADDITIONAL_SENSE_INFORMATION;
                scsiDev.phase = STATUS;

            }
            else if (blocks_till_eof == 0)
            {
                dbgmsg("------ Reached end of tape");
                scsiDev.target->sense.eom = true;
                scsiDev.status = CHECK_CONDITION;
                scsiDev.target->sense.code = BLANK_CHECK;
                scsiDev.target->sense.asc = NO_ADDITIONAL_SENSE_INFORMATION;
                scsiDev.phase = STATUS;
            }
        }
    }
    else
    {
        dbgmsg("------ No image open");
        scsiDev.target->sense.filemark = true;
        scsiDev.status = CHECK_CONDITION;
        scsiDev.target->sense.code = MEDIUM_ERROR;
        scsiDev.target->sense.asc = MEDIUM_NOT_PRESENT;
        scsiDev.phase = STATUS;
    }
}

static void doRewind()
{
    image_config_t &img = *(image_config_t*)scsiDev.target->cfg;
    img.tape_mark_block_offset = 0;
    img.tape_mark_index = 0;
    img.tape_pos = 0;
    img.tape_load_next_file = true;
}

extern "C" int scsiTapeCommand()
{
    image_config_t &img = *(image_config_t*)scsiDev.target->cfg;
    int commandHandled = 1;

    uint8_t command = scsiDev.cdb[0];
    if (command == 0x08)
    {
        // READ6
        bool fixed = scsiDev.cdb[1] & 1;
        bool supress_invalid_length = scsiDev.cdb[1] & 2;

        if (img.quirks == S2S_CFG_QUIRKS_OMTI)
        {
            fixed = true;
        }

        uint32_t length =
            (((uint32_t) scsiDev.cdb[2]) << 16) +
            (((uint32_t) scsiDev.cdb[3]) << 8) +
            scsiDev.cdb[4];

        // Host can request either multiple fixed-length blocks, or a single variable length one.
        // If host requests variable length block, we return one blocklen sized block.
        uint32_t blocklen = scsiDev.target->liveCfg.bytesPerSector;
        uint32_t blocks_to_read = length;
        if (!fixed)
        {
            blocks_to_read = 1;

            bool underlength = (length > blocklen);
            bool overlength = (length < blocklen);
            if (overlength || (underlength && !supress_invalid_length))
            {
                dbgmsg("------ Host requested variable block max ", (int)length, " bytes, blocksize is ", (int)blocklen);
                scsiDev.status = CHECK_CONDITION;
                scsiDev.target->sense.code = ILLEGAL_REQUEST;
                scsiDev.target->sense.asc = INVALID_FIELD_IN_CDB;
                scsiDev.phase = STATUS;
                return 1;
            }
        }


        if (blocks_to_read > 0)
        {
            // Check if this is a .TAP format file
            if (tapIsTapFormat(img)) {
                // For .TAP format, use variable/fixed block reading
                if (fixed) {
                    doTapRead(img, length, true);
                } else {
                    doTapRead(img, length, false);
                }
            } else {
                // Use existing multi-file tape logic
                doTapeRead(blocks_to_read);
            }
        }
    }
    else if (command == 0x0A)
    {
        // WRITE6
        bool fixed = scsiDev.cdb[1] & 1;

        if (img.quirks == S2S_CFG_QUIRKS_OMTI)
        {
            fixed = true;
        }

        uint32_t length =
            (((uint32_t) scsiDev.cdb[2]) << 16) +
            (((uint32_t) scsiDev.cdb[3]) << 8) +
            scsiDev.cdb[4];

        // Host can request either multiple fixed-length blocks, or a single variable length one.
        // Only single block length is supported currently.
        uint32_t blocklen = scsiDev.target->liveCfg.bytesPerSector;
        uint32_t blocks_to_write = length;
        if (!fixed)
        {
            blocks_to_write = 1;

            // For .TAP format, allow variable record lengths
            if (!tapIsTapFormat(img) && length != blocklen)
            {
                dbgmsg("------ Host requested variable block ", (int)length, " bytes, blocksize is ", (int)blocklen);
                scsiDev.status = CHECK_CONDITION;
                scsiDev.target->sense.code = ILLEGAL_REQUEST;
                scsiDev.target->sense.asc = INVALID_FIELD_IN_CDB;
                scsiDev.phase = STATUS;
                return 1;
            }
        }

        if (blocks_to_write > 0)
        {
            if (img.tape_length_mb > 0 && (img.tape_pos + blocks_to_write * blocklen) / 1024 / 1024 > img.tape_length_mb)
            {
                scsiDev.status = CHECK_CONDITION;
                scsiDev.target->sense.code = ILLEGAL_REQUEST;
                scsiDev.target->sense.asc = WRITE_PROTECTED;
                scsiDev.phase = STATUS;
                return 1;
            }

            // Check if this is a .TAP format file
            if (tapIsTapFormat(img)) {
                // For .TAP format, calculate the actual record length
                uint32_t record_length;
                if (fixed) {
                    // Fixed mode: length is number of blocks
                    record_length = length * blocklen;
                } else {
                    // Variable mode: length is the record length directly
                    record_length = length;
                }

                // Start .TAP record write
                tapeMediumStartWrite(record_length, fixed);
            } else {
                // Use existing block-based write
                scsiDiskStartWrite(img.tape_pos, blocks_to_write);
                img.tape_pos += blocks_to_write;
            }
        }
    }
    else if (command == 0x13)
    {
        // VERIFY
        bool fixed = scsiDev.cdb[1] & 1;

        if (img.quirks == S2S_CFG_QUIRKS_OMTI)
        {
            fixed = true;
        }

        bool byte_compare = scsiDev.cdb[1] & 2;
        uint32_t length =
            (((uint32_t) scsiDev.cdb[2]) << 16) +
            (((uint32_t) scsiDev.cdb[3]) << 8) +
            scsiDev.cdb[4];

        if (!fixed)
        {
            length = 1;
        }

        if (byte_compare)
        {
            dbgmsg("------ Verify with byte compare is not implemented");
            scsiDev.status = CHECK_CONDITION;
            scsiDev.target->sense.code = ILLEGAL_REQUEST;
            scsiDev.target->sense.asc = INVALID_FIELD_IN_CDB;
            scsiDev.phase = STATUS;
        }
        else
        {
            // Host requests ECC check, report that it passed.
            scsiDev.status = GOOD;
            scsiDev.phase = STATUS;
            img.tape_pos += length;
        }
    }
    else if (command == 0x19)
    {
        // Erase
        if (tapIsTapFormat(img))
        {
            bool lon = scsiDev.cdb[1] & 1;
            if (lon)
            {
                // Erase from current position to end of tape
                if (!img.file.isWritable())
                {
                    scsiDev.status = CHECK_CONDITION;
                    scsiDev.target->sense.code = ILLEGAL_REQUEST;
                    scsiDev.target->sense.asc = WRITE_PROTECTED;
                }
                else if (img.file.truncate(img.tape_pos))
                {
                    // After truncating, write a new End of Medium marker.
                    tapWriteEOM(img);
                    scsiDev.status = GOOD;
                }
                else
                {
                    scsiDev.status = CHECK_CONDITION;
                    scsiDev.target->sense.code = MEDIUM_ERROR;
                    scsiDev.target->sense.asc = NO_ADDITIONAL_SENSE_INFORMATION;
                }
            }
            else
            {
                // Short erase: write an erase gap marker.
                if (!img.file.isWritable())
                {
                    scsiDev.status = CHECK_CONDITION;
                    scsiDev.target->sense.code = ILLEGAL_REQUEST;
                    scsiDev.target->sense.asc = WRITE_PROTECTED;
                }
                else if (tapWriteEraseGap(img) == TAP_OK)
                {
                    scsiDev.status = GOOD;
                }
                else
                {
                    scsiDev.status = CHECK_CONDITION;
                    scsiDev.target->sense.code = MEDIUM_ERROR;
                    scsiDev.target->sense.asc = NO_ADDITIONAL_SENSE_INFORMATION;
                }
            }
        }
        else
        {
            // Old behavior for non-TAP files
            img.tape_pos = img.scsiSectors;
        }
        scsiDev.phase = STATUS;
    }
    else if (command == 0x01)
    {
        // REWIND
        // Set tape position back to 0.
        doRewind();
    }
    else if (command == 0x05)
    {
        // READ BLOCK LIMITS
        uint32_t blocklen = scsiDev.target->liveCfg.bytesPerSector;
        scsiDev.data[0] = 0; // Reserved
        scsiDev.data[1] = (blocklen >> 16) & 0xFF; // Maximum block length (MSB)
        scsiDev.data[2] = (blocklen >>  8) & 0xFF;
        scsiDev.data[3] = (blocklen >>  0) & 0xFF; // Maximum block length (LSB)
        scsiDev.data[4] = (blocklen >>  8) & 0xFF; // Minimum block length (MSB)
        scsiDev.data[5] = (blocklen >>  8) & 0xFF; // Minimum block length (MSB)
        scsiDev.dataLen = 6;
        scsiDev.phase = DATA_IN;
    }
    else if (command == 0x10)
    {
        // WRITE FILEMARKS
        if (tapIsTapFormat(img)) {
            uint32_t count = scsiDev.cdb[5]; // Number of filemarks to write
            if (count == 0) count = 1; // Default to 1 if not specified

            for (uint32_t i = 0; i < count; i++) {
                tap_result_t result = tapWriteFilemark(img);
                if (result != TAP_OK) {
                    scsiDev.status = CHECK_CONDITION;
                    scsiDev.target->sense.code = MEDIUM_ERROR;
                    scsiDev.target->sense.asc = NO_ADDITIONAL_SENSE_INFORMATION;
                    scsiDev.phase = STATUS;
                    return 1;
                }
            }

            dbgmsg("------ Wrote ", (int)count, " filemark(s) to TAP file");
            scsiDev.status = GOOD;
            scsiDev.phase = STATUS;
        } else {
            // For multi-file tapes, filemarks are implicit
            dbgmsg("------ Filemarks storage not implemented for multi-file tapes, reporting ok");
            scsiDev.status = GOOD;
            scsiDev.phase = STATUS;
        }
    }
    else if (command == 0x11)
    {
        // SPACE
        // Set the tape position forward to a specified offset.
        uint8_t code = scsiDev.cdb[1] & 0x7;
        uint32_t count =
            (((uint32_t) scsiDev.cdb[2]) << 24) +
            (((uint32_t) scsiDev.cdb[3]) << 16) +
            (((uint32_t) scsiDev.cdb[4]) << 8) +
            scsiDev.cdb[5];
        if (tapIsTapFormat(img)) {
            // Handle .TAP format spacing
            tap_result_t result = TAP_OK;

            if (code == 0) {
                // Space records
                result = tapSpaceForward(img, count, false);
            } else if (code == 1) {
                // Space filemarks
                result = tapSpaceForward(img, count, true);
            } else if (code == 3) {
                // Space to end of data - move to end of file
                img.tape_pos = img.file.size();
                result = TAP_OK;
            } else {
                // Unsupported space code
                scsiDev.status = CHECK_CONDITION;
                scsiDev.target->sense.code = ILLEGAL_REQUEST;
                scsiDev.target->sense.asc = INVALID_FIELD_IN_CDB;
                scsiDev.phase = STATUS;
                return 1;
            }

            // Handle result
            if (result == TAP_END_OF_TAPE) {
                scsiDev.target->sense.eom = true;
                scsiDev.status = CHECK_CONDITION;
                scsiDev.target->sense.code = BLANK_CHECK;
                scsiDev.target->sense.asc = 0; // END-OF-DATA DETECTED
                scsiDev.phase = STATUS;
            } else if (result == TAP_FILEMARK) {
                scsiDev.target->sense.filemark = true;
                scsiDev.status = CHECK_CONDITION;
                scsiDev.target->sense.code = NO_SENSE;
                scsiDev.target->sense.asc = NO_ADDITIONAL_SENSE_INFORMATION;
                scsiDev.phase = STATUS;
            } else if (result == TAP_ERROR) {
                scsiDev.status = CHECK_CONDITION;
                scsiDev.target->sense.code = MEDIUM_ERROR;
                scsiDev.target->sense.asc = NO_ADDITIONAL_SENSE_INFORMATION;
                scsiDev.phase = STATUS;
            } else {
                scsiDev.status = GOOD;
                scsiDev.phase = STATUS;
            }
        } else {
            // Handle original multi-file format
            if (code == 0)
            {
                // Blocks.
                uint32_t bytesPerSector = scsiDev.target->liveCfg.bytesPerSector;
                uint32_t capacity = img.file.size() / bytesPerSector;

                if (count < capacity)
                {
                    img.tape_pos = count;
                }
                else
                {
                    scsiDev.status = CHECK_CONDITION;
                    scsiDev.target->sense.code = BLANK_CHECK;
                    scsiDev.target->sense.asc = 0; // END-OF-DATA DETECTED
                    scsiDev.phase = STATUS;
                }
            }
            else if (code == 1)
            {
                // Filemarks.
                // For now just indicate end of data
                scsiDev.status = CHECK_CONDITION;
                scsiDev.target->sense.code = BLANK_CHECK;
                scsiDev.target->sense.asc = 0; // END-OF-DATA DETECTED
                scsiDev.phase = STATUS;
            }
            else if (code == 3)
            {
                // End-of-data.
                scsiDev.status = CHECK_CONDITION;
                scsiDev.target->sense.code = BLANK_CHECK;
                scsiDev.target->sense.asc = 0; // END-OF-DATA DETECTED
                scsiDev.phase = STATUS;
            }
        }
    }
    else if (command == 0x2B)
    {
        // Seek/Locate 10
        uint32_t lba =
            (((uint32_t) scsiDev.cdb[3]) << 24) +
            (((uint32_t) scsiDev.cdb[4]) << 16) +
            (((uint32_t) scsiDev.cdb[5]) << 8) +
            scsiDev.cdb[6];

        doSeek(lba);
    }
    else if (command == 0x34)
    {
        // ReadPosition
        uint32_t lba = img.tape_pos;
        scsiDev.data[0] = 0x00;
        if (lba == 0) scsiDev.data[0] |= 0x80;
        if (lba >= img.scsiSectors) scsiDev.data[0] |= 0x40;
        scsiDev.data[1] = 0x00;
        scsiDev.data[2] = 0x00;
        scsiDev.data[3] = 0x00;
        scsiDev.data[4] = (lba >> 24) & 0xFF; // Next block on tape
        scsiDev.data[5] = (lba >> 16) & 0xFF;
        scsiDev.data[6] = (lba >>  8) & 0xFF;
        scsiDev.data[7] = (lba >>  0) & 0xFF;
        scsiDev.data[8] = (lba >> 24) & 0xFF; // Last block in buffer
        scsiDev.data[9] = (lba >> 16) & 0xFF;
        scsiDev.data[10] = (lba >>  8) & 0xFF;
        scsiDev.data[11] = (lba >>  0) & 0xFF;
        scsiDev.data[12] = 0x00;
        scsiDev.data[13] = 0x00;
        scsiDev.data[14] = 0x00;
        scsiDev.data[15] = 0x00;
        scsiDev.data[16] = 0x00;
        scsiDev.data[17] = 0x00;
        scsiDev.data[18] = 0x00;
        scsiDev.data[19] = 0x00;

        scsiDev.phase = DATA_IN;
        scsiDev.dataLen = 20;
    }
    else
    {
        commandHandled = 0;
    }

    return commandHandled;
}
