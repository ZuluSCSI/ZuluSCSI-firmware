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
#include <ZuluSCSI_platform_config.h>
#include <scsiPhy.h>
#include <assert.h>
#include <algorithm>


extern "C" {
#include <scsi.h>
}

tape_drive_t  **g_tape_drive = nullptr;

// scsiStartRead is defined in ZuluSCSI_disk.cpp for platforms without non-blocking read
extern void scsiStartRead(uint8_t* data, uint32_t count, int *parityError);

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

// initialize a SIMH Tape device
void tapeInit(uint8_t scsi_id)
{
    if (g_tape_drive == nullptr)
    {
        g_tape_drive = new tape_drive_t*[S2S_MAX_TARGETS];
        if (g_tape_drive == nullptr)
            logmsg("Ran out of memory allocating g_tape_drive in ", __FILE__, " on line ", __LINE__ - 2);
        assert(g_tape_drive != nullptr);

        for (uint8_t i = 0; i < S2S_MAX_TARGETS; ++i)
        {
            g_tape_drive[i] = nullptr;
        }
    }

    g_tape_drive[scsi_id] = new tape_drive_t;
    if (g_tape_drive[scsi_id] == nullptr)
        logmsg("Ran out of memory allocating g_tape_drive[", (int) scsi_id,"] on SCSI ID ", (int) scsi_id," in ", __FILE__, " on line ", __LINE__ - 2);
    assert(g_tape_drive[scsi_id] != nullptr);

    tape_drive_t *drive_info =  g_tape_drive[scsi_id];

    // Make sure there is enough memory

    scsiDev.targets[scsi_id].sense.filemark = false;
    scsiDev.targets[scsi_id].sense.eom = false;
    scsiDev.targets[scsi_id].sense.ili = false;

    drive_info->file_pos = 0;
    drive_info->data_pos = 0;
    drive_info->tape_length_mb = 0;
    drive_info->tape_mark_count = 1;
    drive_info->tape_mark_index = 0;
    drive_info->tape_mark_block_offset = 0;
    drive_info->tape_load_next_file = true;

}

// deinitialize a SIMH Tape device
void tapeDeinit(uint8_t scsi_id)
{
    if (scsi_id == 0xFF)
    {
        for (uint8_t id = 0; id < S2S_MAX_TARGETS; id++)
        {
            if (g_tape_drive[id] != nullptr)
            {
                delete g_tape_drive[id];
                g_tape_drive[id] = nullptr;
            }
        }
        delete[] g_tape_drive;
        g_tape_drive = nullptr;

    }
    else
    {
        delete g_tape_drive[scsi_id];
        g_tape_drive[scsi_id] = nullptr;

        bool empty_array = true;
        for (uint8_t id = 0; id < S2S_MAX_TARGETS; id++)
        {
            if (g_tape_drive[id] != nullptr)
            {
                empty_array = false;
                break;
            }
        }
        if (empty_array)
        {
            delete[] g_tape_drive;
            g_tape_drive = nullptr;
        }
    }
}

void setTapeMarkCount(uint8_t scsi_id, uint32_t mark_count)
{
    g_tape_drive[scsi_id]->tape_mark_count = mark_count;
}

void tapeSetIsTap(uint8_t scsi_id, bool is_tap)
{
    g_tape_drive[scsi_id]->tape_is_tap_format = is_tap;
}

bool tapeIsTap()
{
    return g_tape_drive[scsiDev.target->targetId]->tape_is_tap_format;
}

// Read a .TAP record moving forward
tap_result_t tapReadRecordForward(image_config_t &img, tap_record_t &record, uint8_t *buffer, uint32_t buffer_size) {
    tape_drive_t *tape_info = g_tape_drive[img.scsiId & S2S_CFG_TARGET_ID_BITS];
    record.is_error = false;
    record.is_filemark = false;
    record.is_eom = false;

    uint8_t header[4];

    // Check if we're at end of data
     dbgmsg("------ TAP read forward: file_pos=", (int)tape_info->file_pos, " file_size=", (int)img.file.size());
    if (tape_info->file_pos >= img.file.size()) {
        return TAP_END_OF_DATA;
    }

    // Read 4-byte header
    if (!img.file.seek(tape_info->file_pos) || img.file.read(header, sizeof(header)) != sizeof(header)) {
        record.is_error = true;
        return TAP_ERROR;
    }

    uint32_t length_with_class = readLE32(header);

    
    record.record_class = (length_with_class >> 28) & 0xF;
    record.length = length_with_class & 0x0FFFFFFF;

    // Check for special markers
    if (length_with_class == TAP_MARKER_TAPEMARK) {
        record.is_filemark = true;
        tape_info->file_pos += 4;
        scsiDev.target->tapeBOM = 0;
        return TAP_FILEMARK;
    }

    if (length_with_class == TAP_MARKER_END_MEDIUM) {
        record.is_eom = true;
        scsiDev.target->tapeBOM = 0;
        return TAP_END_OF_TAPE;
    }

    // For data records, read the data if buffer provided
    if (record.length > 0) {
        uint32_t data_length = record.length;
        uint32_t padded_length = (data_length + 1) & ~1;  // Round up to even

        // buffer is nullptr when data does not need to read
        if (buffer && buffer_size >= data_length) {
            if (!img.file.seek(tape_info->file_pos + 4) || img.file.read(buffer, data_length) != (ssize_t)data_length) {
                logmsg("------ TAP failed to seek and/or read data");
                record.is_error = true;
                return TAP_ERROR;
            }
        }

        // Verify trailing length
        uint8_t trailer[4];
        if (!img.file.seek(tape_info->file_pos + 4 + padded_length) || img.file.read(trailer, 4) != 4) {
            logmsg("------ TAP failed to seek and/or read trailer");
            record.is_error = true;
            return TAP_ERROR;
        }

        uint32_t trailing_length = readLE32(trailer) & 0x0FFFFFFF;
        if (trailing_length != record.length) {
            logmsg("------ TAP record length mismatch: header=", (int)record.length, " trailer=", (int)trailing_length);
            record.is_error = true;
            return TAP_ERROR;
        }

        // Move past this record
        tape_info->file_pos += 8 + padded_length;
        tape_info->data_pos += data_length;
        scsiDev.target->tapeBOM = 0;
    } else {
        // Zero-length record, move past header
        tape_info->file_pos += 4;
        scsiDev.target->tapeBOM = 0;
    }

    return TAP_OK;
}

// Read a .TAP record moving backward
tap_result_t tapReadRecordBackward(image_config_t &img, tap_record_t &record, uint8_t *buffer, uint32_t buffer_size) {
    tape_drive_t *tape_info = g_tape_drive[img.scsiId & S2S_CFG_TARGET_ID_BITS];
    record.is_error = false;
    record.is_filemark = false;
    record.is_eom = false;

    // Check if we're at beginning of tape
    if (tape_info->file_pos == 0) {
        scsiDev.target->tapeBOM = 1;
        return TAP_BEGINNING_OF_TAPE;
    }

    uint8_t trailer[4];

    // Read 4 bytes before current position (trailing length)
    if (tape_info->file_pos < 4 || !img.file.seek(tape_info->file_pos - 4) || img.file.read(trailer, 4) != 4) {
        record.is_error = true;
        return TAP_ERROR;
    }

    uint32_t length_with_class = readLE32(trailer);

    // Check for special markers
    if (length_with_class == TAP_MARKER_TAPEMARK) {
        record.is_filemark = true;
        tape_info->file_pos -= 4;
        scsiDev.target->tapeBOM = 0;
        return TAP_FILEMARK;
    }

    if (length_with_class == TAP_MARKER_END_MEDIUM) {
        record.is_eom = true;
        tape_info->file_pos -= 4;
        scsiDev.target->tapeBOM = 0;
        return TAP_END_OF_TAPE;
    }

    record.record_class = (length_with_class >> 28) & 0xF;
    record.length = length_with_class & 0x0FFFFFFF;

    if (record.length > 0) {
        uint32_t padded_length = (record.length + 1) & ~1;
        uint32_t total_length = 8 + padded_length;

        if (tape_info->file_pos < total_length) {
            record.is_error = true;
            return TAP_ERROR;
        }

        // Move to start of record
        tape_info->file_pos -= total_length;
        tape_info->data_pos -= record.length;

        // we are at the begining of the tape, beginning of media
        if (tape_info->file_pos == 0)
            scsiDev.target->tapeBOM = 1;

        // Verify header length matches
        uint8_t header[4];
        if (!img.file.seek(tape_info->file_pos) || img.file.read(header, 4) != 4) {
            logmsg("------ TAP backward record header read or seek error");
            record.is_error = true;
            return TAP_ERROR;
        }

        uint32_t header_length = readLE32(header) & 0x0FFFFFFF;
        if (header_length != record.length) {
            logmsg("------ TAP backward record length mismatch: header=", (int)header_length, " trailer=", (int)record.length);
            record.is_error = true;
            return TAP_ERROR;
        }

        // Read data if buffer provided
        if (buffer && buffer_size >= record.length) {
            if (!img.file.seek(tape_info->file_pos + 4) || img.file.read(buffer, record.length) != (ssize_t)record.length) {
                logmsg("------ TAP backward record data read or seek error");
                record.is_error = true;
                return TAP_ERROR;
            }
        }
    } else {
        // Zero-length record
        tape_info->file_pos -= 4;
        // we are at the begining of the tape, beginning of media
        if (tape_info->file_pos == 0)
            scsiDev.target->tapeBOM = 1;
        
    }

    return TAP_OK;
}

// Write a .TAP data record
tap_result_t tapWriteRecord(image_config_t &img, const uint8_t *data, uint32_t length) {
    tape_drive_t *tape_info = g_tape_drive[img.scsiId & S2S_CFG_TARGET_ID_BITS];
    uint32_t padded_length = (length + 1) & ~1;  // Round up to even
    uint8_t header[4], trailer[4];

    // Write header (length with class 0 for good data)
    writeLE32(header, length);
    if (!img.file.seek(tape_info->file_pos) || img.file.write(header, 4) != 4) {
        return TAP_ERROR;
    }

    // Write data
    if (length > 0) {
        if (!img.file.seek(tape_info->file_pos + 4) || img.file.write(data, length) != (ssize_t)length) {
            return TAP_ERROR;
        }

        // Write padding byte if needed
        if (padded_length > length) {
            uint8_t pad = 0;
            if (!img.file.seek(tape_info->file_pos + 4 + length) || img.file.write(&pad, 1) != 1) {
                return TAP_ERROR;
            }
        }
    }

    // Write trailer (same length)
    writeLE32(trailer, length);
    if (!img.file.seek(tape_info->file_pos + 4 + padded_length) || img.file.write(trailer, 4) != 4) {
        return TAP_ERROR;
    }

    // Move past this record
    tape_info->file_pos += 8 + padded_length;
    tape_info->data_pos += length;

    return TAP_OK;
}

// Write a filemark
tap_result_t tapWriteFilemark(image_config_t &img) {
    tape_drive_t *tape_info = g_tape_drive[img.scsiId & S2S_CFG_TARGET_ID_BITS];
    uint8_t marker[4];
    writeLE32(marker, TAP_MARKER_TAPEMARK);

    if (!img.file.seek(tape_info->file_pos) || img.file.write(marker, 4) != 4) {
        return TAP_ERROR;
    }
    img.file.flush();
    tape_info->file_pos += 4;
    scsiDev.target->tapeBOM = 0;
    return TAP_OK;
}

// Write an end-of-medium mark
tap_result_t tapWriteEOM(image_config_t &img) {
    tape_drive_t *tape_info = g_tape_drive[img.scsiId & S2S_CFG_TARGET_ID_BITS];
    uint8_t marker[4];
    writeLE32(marker, TAP_MARKER_END_MEDIUM);

    if (!img.file.seek(tape_info->file_pos) || img.file.write(marker, 4) != 4) {
        return TAP_ERROR;
    }

    tape_info->file_pos += 4;
    return TAP_OK;
}

// Write an erase gap
tap_result_t tapWriteEraseGap(image_config_t &img) {
    tape_drive_t *tape_info = g_tape_drive[img.scsiId & S2S_CFG_TARGET_ID_BITS];
    uint8_t marker[4];
    writeLE32(marker, TAP_MARKER_ERASE_GAP);

    if (!img.file.seek(tape_info->file_pos) || img.file.write(marker, 4) != 4)
    {
        return TAP_ERROR;
    }

    tape_info->file_pos += 4;
    return TAP_OK;
}

// Space forward by records or filemarks
tap_result_t tapSpaceForward(image_config_t &img, uint32_t &actual, uint32_t count, bool filemarks) 
{
    uint32_t blocksize = scsiDev.target->liveCfg.bytesPerSector;
    
    // variable blocks (blocksize == 0) and block counting (!filemarks) not allowed
    if (!filemarks && blocksize == 0)
        return TAP_ERROR;

    uint32_t records_read = 0;
    tap_record_t record;
    actual = 0;
    while (actual < count) {
        tap_result_t result = tapReadRecordForward(img, record, nullptr, 0);

        if (result == TAP_END_OF_TAPE) {
            return TAP_END_OF_TAPE;
        }

        if (result == TAP_ERROR) {
            return TAP_ERROR;
        }

        if (filemarks) {
            // Spacing filemarks - continue count when we hit one
             if (result == TAP_FILEMARK)
                ++actual;
        } else {
            if (result == TAP_OK)
            {
                // allow for smaller record.length than blocksize to be read until
                // if fills the blocksize, error on overflows 
                records_read += record.length;
                if (records_read > blocksize) {
                    return TAP_ERROR;
                }
                if (records_read == blocksize)
                {
                    records_read = 0;
                    ++actual;
                }
            }
            else if (result == TAP_FILEMARK)
            {
                return TAP_FILEMARK;
            }
        }
    }

    return TAP_OK;
}

// Space backward by records or filemarks
tap_result_t tapSpaceBackward(image_config_t &img, uint32_t &actual, uint32_t count, bool filemarks) {
    uint32_t blocksize = scsiDev.target->liveCfg.bytesPerSector;
    // variable blocks (blocksize == 0) and block counting (!filemarks) not allowed
    if (!filemarks && blocksize == 0)
        return TAP_ERROR;
    uint32_t records_read = 0;
    tap_record_t record;
    
    actual = 0;
    while (actual < count) {
        tap_result_t result = tapReadRecordBackward(img, record, nullptr, 0);

        if (result == TAP_BEGINNING_OF_TAPE) {
            return TAP_BEGINNING_OF_TAPE;
        }

        if (result == TAP_ERROR) {
            return TAP_ERROR;
        }
        
        
        if (filemarks) {
            if (result == TAP_FILEMARK)
                ++actual;
        }
        else
        {
            if (result == TAP_OK)
            {
                records_read += record.length;

                if (records_read > blocksize) 
                {
                    return TAP_ERROR;
                }

                if (records_read == blocksize)
                {
                    records_read = 0;
                    ++actual;
                }
            }
            else if (result == TAP_FILEMARK)
            {
                return TAP_FILEMARK;
            }
        }
    }
    return TAP_OK;
}

static tap_result_t tapSpaceEndOfData(image_config_t &img)
{
    // cannot simply set tape_info->file_pos to end of file because
    // tape_info->data_pos will not increment correctly
    tape_drive_t *tape_info = g_tape_drive[img.scsiId & S2S_CFG_TARGET_ID_BITS];
    uint64_t end_of_tape = tape_info->tape_length_mb * 1024 * 1024;
    tap_result_t result;
    tap_record_t record;
    while (end_of_tape == 0 || tape_info->data_pos < end_of_tape)
    {
        result = tapReadRecordForward(img, record, nullptr, 0);
        if (result == TAP_END_OF_TAPE || result == TAP_ERROR)
            return result;
        if (result == TAP_END_OF_DATA)
            return TAP_OK;
    }
    return  TAP_END_OF_TAPE;
}

// Rewind to beginning of tape
tap_result_t tapRewind(image_config_t &img) {
    tape_drive_t *tape_info = g_tape_drive[img.scsiId & S2S_CFG_TARGET_ID_BITS];
    tape_info->file_pos = 0;
    tape_info->data_pos = 0;
    return TAP_OK;
}

// State tracking for .TAP format writes
struct tap_transfer_t {
    uint32_t record_length;     // Total length of record being written
    uint32_t bytes_written;     // Bytes written so far
    uint64_t data_written;      // Just counting data bytes, no metadata 
    bool is_fixed_mode;         // Fixed vs variable block mode
    bool header_written;        // Has 4-byte header been written?
    uint64_t file_pos_start;    // File position at start of record

    // Fields for data transfer management (similar to g_disk_transfer)
    uint8_t* buffer;            // Buffer for data transfer
    uint32_t bytes_scsi;        // Total SCSI buffer size
    uint32_t bytes_sd;          // Bytes processed so far
    uint32_t bytes_scsi_started; // Bytes started in SCSI transfer
    uint32_t sd_transfer_start; // Start position for SD transfer
    uint32_t current_block;     // Current block being written in fixed block
    int parityError;            // Parity error flag
};

static tap_transfer_t g_tap_transfer;

// .TAP-aware read function
static void doTapRead(image_config_t &img, uint32_t blocks, bool fixed, bool suppress_invalid_length) {
    tap_record_t record;
    uint32_t block_size;
    if (fixed)
        block_size =  scsiDev.target->liveCfg.bytesPerSector;
    else
    {
        // with variable sector sizes, the blocks is the number of bytes of the sector and and only one sector is transferred 
        block_size = blocks;
        blocks = 1;
    }
    // Fixed block mode - read - if record length
    scsiDev.phase = DATA_IN;
    scsiDev.dataLen = 0;
    scsiDev.dataPtr = 0;
    scsiEnterPhase(DATA_IN);

    // Use two buffers alternately for formatting sector data
    if (block_size > scsiTapeMaxSectorSize() || (block_size * 2) > sizeof(scsiDev.data))
    {
        if (block_size > scsiTapeMaxSectorSize())
        {
            logmsg("doTapRead() block size, ", (int) block_size, ", is larger than the max size: ", (int) scsiTapeMaxSectorSize());
        }
        if ((block_size * 2) > sizeof(scsiDev.data))
        {
            logmsg("doTapRead() block size, ", (int) block_size, ", is larger than the allocated buffer size: ", (int) sizeof(scsiDev.data) / 2);
        }
        scsiDev.status = CHECK_CONDITION;
        scsiDev.target->sense.code = MEDIUM_ERROR;
        scsiDev.target->sense.asc = NO_ADDITIONAL_SENSE_INFORMATION;
        scsiDev.phase = STATUS;
        return;
    }

    uint8_t *buf0 = scsiDev.data;
    uint8_t *buf1 = scsiDev.data + block_size;
    tape_drive_t *tape_info = g_tape_drive[img.scsiId & S2S_CFG_TARGET_ID_BITS];
    // Format the sectors for transfer
    for (uint32_t block = 0; block < blocks; block++)
    {
        platform_poll();
        diskEjectButtonUpdate(false);

        // Verify that previous write using this buffer has finished
        uint8_t *buf = ((block & 1) ? buf1 : buf0);
        uint32_t start = millis();
        while (!scsiIsWriteFinished(buf + block_size - 1) && !scsiDev.resetFlag)
        {
            if ((uint32_t)(millis() - start) > 5000)
            {
                logmsg("doTapRead() fixed length timeout waiting for previous to finish");
                scsiDev.resetFlag = 1;
            }
            platform_poll();
            diskEjectButtonUpdate(false);
        }

        if (scsiDev.resetFlag)
            break;

        uint32_t bytes_read = 0;

        // Read record lengths that are either the full size of the block size or record lengths that
        // divide evenly to fill the blocksize
        do
        {
            tap_result_t result = tapReadRecordForward(img, record, buf + bytes_read, block_size - bytes_read);
            if (result != TAP_OK && block > 0)
                scsiFinishWrite();

            if (result == TAP_FILEMARK) {
                uint32_t info = fixed ? (blocks - block) : block_size;
                if (fixed)
                {
                    dbgmsg("------ TAP fixed read hit filemark after ", (int) block,
                            " block(s), residual=", (int)(info),
                            " file_pos=", (int)tape_info->file_pos);
                }
                else
                {
                    dbgmsg("------ TAP variable read hit filemark after ", (int) bytes_read,
                            " bytes(s), residual=", (int)(info),
                            " file_pos=", (int)tape_info->file_pos);
                }
                scsiDev.target->sense.filemark = true;
                scsiDev.status = CHECK_CONDITION;
                scsiDev.target->sense.code = NO_SENSE;
                scsiDev.target->sense.asc = NO_ADDITIONAL_SENSE_INFORMATION;
                scsiDev.target->sense.info = info;
                scsiDev.phase = STATUS;
                return;
            } else if (result == TAP_END_OF_DATA) {
                dbgmsg("------ TAP ", fixed ? "fixed" : "variable",
                    " read hit end-of-data at file_pos=", (int)tape_info->file_pos);
                scsiDev.status = CHECK_CONDITION;
                scsiDev.target->sense.code = BLANK_CHECK;
                scsiDev.target->sense.asc = NO_ADDITIONAL_SENSE_INFORMATION;
                scsiDev.target->sense.info = fixed ? (blocks - block) : block_size;
                scsiDev.phase = STATUS;
                return;
            } else if (result == TAP_END_OF_TAPE) {
                dbgmsg("------ TAP ", fixed ? "fixed" : "variable",
                    " read hit end-of-tape at file_pos=", (int)tape_info->file_pos);
                scsiDev.target->sense.eom = true;
                scsiDev.status = CHECK_CONDITION;
                scsiDev.target->sense.code = MEDIUM_ERROR;
                scsiDev.target->sense.asc = NO_ADDITIONAL_SENSE_INFORMATION;
                scsiDev.target->sense.info = fixed ? (blocks - block) : block_size;
                scsiDev.phase = STATUS;
                return;
            } else if (result == TAP_ERROR) {
                dbgmsg("------ TAP ", fixed ? "fixed" : "variable",
                    " read hit parser/media error at file_pos=", (int)tape_info->file_pos);
                scsiDev.status = CHECK_CONDITION;
                scsiDev.target->sense.code = MEDIUM_ERROR;
                scsiDev.target->sense.asc = NO_ADDITIONAL_SENSE_INFORMATION;
                scsiDev.target->sense.info = fixed ? (blocks - block) : block_size;
                scsiDev.phase = STATUS;
                return;
            }

            bytes_read += record.length;
        } while (bytes_read < block_size);

        // Check if the buffer contains a full block
        if (bytes_read != block_size) {
            if (block > 0)
                scsiFinishWrite();
            logmsg("------ TAP block length mismatch: block size=", (int)block_size, " bytes read=", (int)bytes_read);
            scsiDev.status = CHECK_CONDITION;
            scsiDev.target->sense.code = ILLEGAL_REQUEST;
            scsiDev.target->sense.asc = INVALID_FIELD_IN_CDB;
            scsiDev.phase = STATUS;
            return;
        }

        scsiStartWrite(buf, block_size);

        // Reset the watchdog while the transfer is progressing.
        // If the host stops transferring, the watchdog will eventually expire.
        // This is needed to avoid hitting the watchdog if the host performs
        // a large transfer compared to its transfer speed.
        platform_reset_watchdog();
    }

    scsiFinishWrite();
}

// Start a .TAP format write operation
void tapMediumStartWrite(uint32_t length, bool fixed) {
    image_config_t &img = *(image_config_t*)scsiDev.target->cfg;
    tape_drive_t *tape_info = g_tape_drive[img.scsiId & S2S_CFG_TARGET_ID_BITS];
    uint16_t block_size = scsiDev.target->liveCfg.bytesPerSector;
    
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
    g_tap_transfer.record_length = fixed ? block_size : length;
    g_tap_transfer.bytes_written = 0;
    g_tap_transfer.is_fixed_mode = fixed;
    g_tap_transfer.file_pos_start = tape_info->file_pos;
    g_tap_transfer.data_written = 0;

    // Set up SCSI transfer similar to scsiDiskStartWrite
    transfer.blocks = fixed ? length : 1;
    transfer.multiBlock = fixed && (transfer.blocks > 0);
    transfer.lba = 0;  // Not used for .TAP
    transfer.currentBlock = 0;

    scsiDev.phase = DATA_OUT;
    scsiDev.dataLen = 0;
    scsiDev.dataPtr = 0;
    scsiDev.postDataOutHook = nullptr;
}

void tapDataOut_callback(uint32_t bytes_complete)
{
    // For best performance, do SCSI reads in blocks of 4 or more bytes
    bytes_complete &= ~3;

    if (g_tap_transfer.bytes_scsi_started < g_tap_transfer.bytes_scsi)
    {
        // How many bytes remaining in the transfer?
        uint32_t remain = g_tap_transfer.bytes_scsi - g_tap_transfer.bytes_scsi_started;
        uint32_t len = remain;

        // Split read so that it doesn't wrap around buffer edge
        uint32_t bufsize = sizeof(scsiDev.data);
        uint32_t start = (g_tap_transfer.bytes_scsi_started % bufsize);
        if (start + len > bufsize)
            len = bufsize - start;

        // Apply platform-specific optimized transfer sizes
        if (len > PLATFORM_OPTIMAL_SCSI_READ_BLOCK_SIZE)
        {
            len = PLATFORM_OPTIMAL_SCSI_READ_BLOCK_SIZE;
        }

        // Don't overwrite data that has not yet been written to SD card
        uint32_t sd_ready_cnt = g_tap_transfer.bytes_sd + bytes_complete;
        if (g_tap_transfer.bytes_scsi_started + len > sd_ready_cnt + bufsize)
            len = sd_ready_cnt + bufsize - g_tap_transfer.bytes_scsi_started;

        // Keep transfers a multiple of sector size.
        // Macintosh SCSI driver seems to get confused if we have a delay
        // in middle of a sector.
        uint32_t bytesPerSector = g_tap_transfer.record_length;
        if (remain >= bytesPerSector && len % bytesPerSector != 0)
        {
            len -= len % bytesPerSector;
        }

        if (len == 0)
            return;

        // dbgmsg("SCSI read ", (int)start, " + ", (int)len);
        scsiStartRead(&scsiDev.data[start], len, &g_tap_transfer.parityError);
        g_tap_transfer.bytes_scsi_started += len;
    }

}

void tapeTapDataOut()
{
    image_config_t &img = *(image_config_t*)scsiDev.target->cfg;
    tape_drive_t *tape_info = g_tape_drive[img.scsiId & S2S_CFG_TARGET_ID_BITS];
    uint32_t blockcount = (transfer.blocks - transfer.currentBlock);
    uint32_t bytesPerSector = g_tap_transfer.record_length;

    // If we are using non-power-of-two sector size, wrapping around
    // the buffer edge doesn't work out. Instead limit the transfer
    // to a smaller section and re-enter diskDataOut().
    uint32_t blocksPerBuffer = sizeof(scsiDev.data) / bytesPerSector;
    if (blockcount > blocksPerBuffer &&
        blocksPerBuffer * bytesPerSector != sizeof(scsiDev.data))
    {
        blockcount = blocksPerBuffer;
    }

    g_tap_transfer.buffer = scsiDev.data;
    g_tap_transfer.bytes_scsi = blockcount * bytesPerSector;
    g_tap_transfer.bytes_sd = 0;
    g_tap_transfer.bytes_scsi_started = 0;
    g_tap_transfer.sd_transfer_start = 0;
    g_tap_transfer.parityError = 0;

    if (!img.file.seek(tape_info->file_pos))
    {
        logmsg("SD card seek failed: ", SD.sdErrorCode());
        scsiDev.status = CHECK_CONDITION;
        scsiDev.target->sense.code = MEDIUM_ERROR;
        scsiDev.target->sense.asc = NO_ADDITIONAL_SENSE_INFORMATION;
        scsiDev.phase = STATUS;
        return;
    }

    while (g_tap_transfer.bytes_sd < g_tap_transfer.bytes_scsi
        && scsiDev.phase == DATA_OUT
        && !scsiDev.resetFlag)
    {
        platform_poll();
        diskEjectButtonUpdate(false);
        
        // Figure out how many contiguous bytes are available for writing to SD card.
        uint32_t bufsize = sizeof(scsiDev.data);
        uint32_t start = g_tap_transfer.bytes_sd % bufsize;
        uint32_t len = 0;

        uint32_t available = g_tap_transfer.bytes_scsi_started - g_tap_transfer.bytes_sd;
        if (start + available > bufsize)
        {
            available = bufsize - start;
        }
        // Count number of finished sectors
        if (scsiIsReadFinished(&scsiDev.data[start + available - 1]))
        {
            len = available;
        }
        else
        {
            while (len < available && scsiIsReadFinished(&scsiDev.data[start + len + SD_SECTOR_SIZE - 1]))
            {
                len += SD_SECTOR_SIZE;
            }
        }

        // In case the last sector is partial (256 byte SCSI sectors)
        if (len > available)
        {
            len = available;
        }
        // Apply platform-specific write size blocks for optimization
        if (len > PLATFORM_OPTIMAL_MAX_SD_WRITE_SIZE)
        {
            len = PLATFORM_OPTIMAL_MAX_SD_WRITE_SIZE;
        }

        uint32_t remain_in_transfer = g_tap_transfer.bytes_scsi - g_tap_transfer.bytes_sd;
        if (len < bufsize - start && len < remain_in_transfer)
        {
            // Use large write blocks in middle of transfer and smaller at the end of transfer.
            // This improves performance for large writes and reduces latency at end of request.
            uint32_t min_write_size = PLATFORM_OPTIMAL_MIN_SD_WRITE_SIZE;
            if (remain_in_transfer <= PLATFORM_OPTIMAL_MAX_SD_WRITE_SIZE)
            {
                min_write_size = PLATFORM_OPTIMAL_LAST_SD_WRITE_SIZE;
            }

            if (len < min_write_size)
            {
                len = 0;
            }
        }
        
        if (len == 0)
        {
            // Nothing ready to transfer, check if we can read more from SCSI bus
            tapDataOut_callback(0);
        }
        else
        {
            
            // Start writing to SD card and simultaneously start new SCSI transfers
            // when buffer space is freed.
            
            uint8_t *buf = &scsiDev.data[start];
            g_tap_transfer.sd_transfer_start = start;
            
            uint32_t end_of_data_to_write =  len;
            uint32_t record_length = g_tap_transfer.record_length;
            uint64_t data_written_start = g_tap_transfer.data_written;

            uint8_t record_length_metadata[4];
            writeLE32(record_length_metadata, record_length);

            while (g_tap_transfer.data_written - data_written_start < end_of_data_to_write)
            {
                uint32_t end_of_next_data_record =  record_length - (g_tap_transfer.data_written % record_length);
                // write header
                if ((g_tap_transfer.data_written % record_length) == 0)
                {                        if (img.file.write(record_length_metadata, sizeof(record_length_metadata)) != sizeof(record_length_metadata))
                    {
                        goto write_error;
                    }
                    tape_info->file_pos += sizeof(record_length_metadata);
                }


                uint32_t data_to_write_len = std::min(end_of_next_data_record, len);

                // Finalize transfer on SCSI side
                scsiFinishRead(buf, data_to_write_len, &g_tap_transfer.parityError);

                // Check parity error status before writing to SD card
                if (g_tap_transfer.parityError)
                {
                    scsiDev.status = CHECK_CONDITION;
                    scsiDev.target->sense.code = ABORTED_COMMAND;
                    scsiDev.target->sense.asc = SCSI_PARITY_ERROR;
                    scsiDev.phase = STATUS;
                    break;
                }

                // write data
                platform_set_sd_callback(tapDataOut_callback, buf);
                if (img.file.write(buf, data_to_write_len) != data_to_write_len)
                {
                    goto write_error;
                }
                g_tap_transfer.data_written += data_to_write_len;
                tape_info->file_pos +=  data_to_write_len;
                tape_info->data_pos += data_to_write_len;
                platform_set_sd_callback(NULL, NULL);
                g_tap_transfer.bytes_sd += data_to_write_len;
                buf += data_to_write_len;

                // write trailer
                if (data_to_write_len == end_of_next_data_record)
                {
                    // round to even position
                    if (tape_info->file_pos & 1)
                    { 
                        uint8_t write_zero = 0;
                        if (img.file.write(&write_zero, 1) == 1)
                            tape_info->file_pos += 1;
                        else
                            goto write_error;
                    }
                    if (img.file.write(record_length_metadata, sizeof(record_length_metadata)) != sizeof(record_length_metadata))
                    {
                        goto write_error;
                    }
                    tape_info->file_pos += sizeof(record_length_metadata);
                    transfer.currentBlock += 1;
                    // After a record has been written, invalidate tape at beginning of media
                    scsiDev.target->tapeBOM = 0;
                }

            }
        }
    }

    // Release SCSI bus
    scsiFinishRead(NULL, 0, &g_tap_transfer.parityError);

    scsiDev.dataPtr = scsiDev.dataLen = 0;

    // Verify that all data has been flushed to disk from SdFat cache.
    img.file.flush();
    return;

write_error:
    logmsg("SD card write failed: ", SD.sdErrorCode());
    scsiDev.status = CHECK_CONDITION;
    scsiDev.target->sense.code = MEDIUM_ERROR;
    scsiDev.target->sense.asc = NO_ADDITIONAL_SENSE_INFORMATION;
    scsiDev.phase = STATUS;
    img.file.flush();
}

static void doLocate(uint32_t lba)
{
    image_config_t &img = *(image_config_t*)scsiDev.target->cfg;
    tape_drive_t *tape_info = g_tape_drive[img.scsiId & S2S_CFG_TARGET_ID_BITS];
    uint32_t bytesPerSector = scsiDev.target->liveCfg.bytesPerSector;
    if (bytesPerSector == 0)
    {
        logmsg("Locate unsupported when tape is set to variable block length");
        scsiDev.status = CHECK_CONDITION;
        scsiDev.target->sense.code = ILLEGAL_REQUEST;
        scsiDev.target->sense.asc = NO_ADDITIONAL_SENSE_INFORMATION;
        scsiDev.phase = STATUS;
        return;
    }

    uint32_t block = 0;
    tape_info->file_pos = 0;
    tape_info->data_pos = 0;
    while (block < lba)
    {
        tap_record_t record;
        uint32_t bytes_read = 0;
        platform_poll();
        diskEjectButtonUpdate(false);
        do
        {
            tap_result_t result = tapReadRecordForward(img, record, nullptr, 0);
            if (result == TAP_FILEMARK) {
                continue;
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

            bytes_read += record.length;
        } while (bytes_read < bytesPerSector);

        // check if sum of simh tap records read match block size
        if (bytes_read != bytesPerSector) {
            logmsg("------ TAP block length mismatch: block size=", (int)bytesPerSector, " bytes read=", (int)bytes_read);
            scsiDev.status = CHECK_CONDITION;
            scsiDev.target->sense.code = ILLEGAL_REQUEST;
            scsiDev.target->sense.asc = INVALID_FIELD_IN_CDB;
            scsiDev.phase = STATUS;
            return;
        }
        ++block;
        platform_reset_watchdog();
    }

    scsiDev.status = GOOD;
    scsiDev.phase = STATUS;
}

static void doTapeRead(uint32_t blocks)
{
    image_config_t &img = *(image_config_t*)scsiDev.target->cfg;
    uint8_t scsi_id = img.scsiId & S2S_CFG_TARGET_ID_BITS;
    tape_drive_t *tape_info = g_tape_drive[scsi_id];

    uint32_t tape_length_mb = g_scsi_settings.getDevice(scsi_id)->tapeLengthMB;
    uint32_t block_size = scsiDev.target->liveCfg.bytesPerSector;
    if (tape_length_mb > 0 && block_size > 0)
    {
        uint32_t capacity_mb = tape_length_mb;
        uint64_t capacity_bytes = (uint64_t)capacity_mb * 1024 * 1024;
        uint32_t capacity_blocks = capacity_bytes / block_size;

        if (tape_info->data_pos >= capacity_blocks)
        {
            scsiDev.target->sense.eom = true;
            scsiDev.status = CHECK_CONDITION;
            scsiDev.target->sense.code = BLANK_CHECK;
            scsiDev.target->sense.asc = NO_ADDITIONAL_SENSE_INFORMATION;
            scsiDev.phase = STATUS;
            return;
        }

        if (tape_info->file_pos + blocks > capacity_blocks)
        {
            blocks = capacity_blocks - tape_info->file_pos;
            scsiDev.target->sense.eom = true;
        }
    }

    uint32_t capacity_lba = img.get_capacity_lba();

    if (tape_info->tape_load_next_file && img.bin_container.isOpen())
    {
        // multifile tape - multiple file markers
        char dir_name[MAX_FILE_PATH + 1];
        char current_filename[MAX_FILE_PATH + 1] = {0};
        char next_filename[MAX_FILE_PATH + 1] = {0};
        int filename_len = 0;
        img.bin_container.getName(dir_name, sizeof(dir_name));
        img.file.getFilename(current_filename, sizeof(current_filename));
        tape_info->tape_load_next_file = false;
        // load first file in directory or load next file

        filename_len = findNextImageAfter(img, dir_name, current_filename, next_filename, sizeof(next_filename), true);
        if (filename_len > 0 && img.file.selectImageFile(next_filename))
        {
            if (tape_info->tape_mark_index > 0)
            {
                tape_info->tape_mark_block_offset += capacity_lba;
            }
            else
            {
                tape_info->tape_mark_block_offset = 0;
            }
            capacity_lba = img.get_capacity_lba();

            dbgmsg("------ Read tape loaded file ", next_filename, " has ", (int) capacity_lba, " sectors with filemark ", (int) tape_info->tape_mark_index ," at the end");
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
        if (unlikely(((uint64_t) tape_info->file_pos) - tape_info->tape_mark_block_offset + blocks >= capacity_lba))
        {
            // reading past a file, set blocks to end of file
            blocks_till_eof =  capacity_lba - (tape_info->file_pos - tape_info->tape_mark_block_offset);
            passed_filemarker = true;
            // SCSI-2 Spec: "If the fixed bit is one, the information field shall be set to the requested transfer length minus the
            //               actual number of blocks read (not including the filemark)"
            scsiDev.target->sense.info = blocks - blocks_till_eof;
            dbgmsg("------ Read tape went past file marker, blocks left to be read ", (int) blocks_till_eof, " out of ", (int) blocks, " sense info set to ", (int) scsiDev.target->sense.info);

            blocks = blocks_till_eof;
        }

        if (blocks > 0)
        {
            dbgmsg("------ Read tape ", (int)blocks, "x", (int)scsiDev.target->liveCfg.bytesPerSector, " tape position ",(int)tape_info->file_pos,
                            " file position ", (int)(tape_info->file_pos - tape_info->tape_mark_block_offset), " ends with file mark ",
                            (int)(tape_info->tape_mark_index + 1), "/", (int) tape_info->tape_mark_count, passed_filemarker ? " reached" : " not reached");
            scsiDiskStartRead(tape_info->file_pos - tape_info->tape_mark_block_offset, blocks);
            tape_info->file_pos += blocks;
        }

        if (passed_filemarker)
        {
            if (tape_info->tape_mark_index < tape_info->tape_mark_count)
            {
                tape_info->tape_mark_index++;
                if(tape_info->tape_mark_index < tape_info->tape_mark_count)
                    tape_info->tape_load_next_file = true;

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
    tape_drive_t *tape_info = g_tape_drive[img.scsiId & S2S_CFG_TARGET_ID_BITS];
    tape_info->tape_mark_block_offset = 0;
    tape_info->tape_mark_index = 0;
    tape_info->file_pos = 0;
    tape_info->data_pos = 0;
    tape_info->tape_load_next_file = true;
    scsiDev.target->tapeBOM = 1;
}

static void doTapVerify(uint32_t length, bool fixed)
{
    image_config_t &img = *(image_config_t*)scsiDev.target->cfg;
    // Convert fixed length to number of byte to verify, this makes it easier
    // test tap record.length smaller than the blocksize
    uint32_t bytesPerSector = fixed ? scsiDev.target->liveCfg.bytesPerSector : length;
    uint32_t length_bytes = fixed ? length * bytesPerSector : length;
    uint32_t pos = 0;

    while (pos < length_bytes)
    {
        tap_record_t record;
        uint32_t bytes_read = 0;
        platform_poll();
        diskEjectButtonUpdate(false);
        do
        {
            tap_result_t result = tapReadRecordForward(img, record, nullptr, 0);
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

            bytes_read += record.length;
        } while (bytes_read < bytesPerSector);

        // check if sum of simh tap records read match block size
        if (bytes_read != bytesPerSector) {
            logmsg("------ TAP block length mismatch: block size=", (int)bytesPerSector, " bytes read=", (int)bytes_read);
            scsiDev.status = CHECK_CONDITION;
            scsiDev.target->sense.code = ILLEGAL_REQUEST;
            scsiDev.target->sense.asc = INVALID_FIELD_IN_CDB;
            scsiDev.phase = STATUS;
            return;
        }
        pos += bytes_read;
        platform_reset_watchdog();
    }
    scsiDev.status = GOOD;
    scsiDev.phase = STATUS;
}

int scsiTapeMaxSectorSize()
{
    image_config_t &img = *(image_config_t*)scsiDev.target->cfg;
    tape_drive_t *tape_info = g_tape_drive[img.scsiId & S2S_CFG_TARGET_ID_BITS];
    return tape_info->tape_is_tap_format ? TAPE_TAP_BLOCK_SIZE_MAX : TAPE_BLOCK_SIZE_MAX;
}

int scsiTapeMinSectorSize()
{
    image_config_t &img = *(image_config_t*)scsiDev.target->cfg;
    tape_drive_t *tape_info = g_tape_drive[img.scsiId & S2S_CFG_TARGET_ID_BITS];
    return tape_info->tape_is_tap_format ? TAPE_TAP_BLOCK_SIZE_MIN : TAPE_BLOCK_SIZE_MIN;
}

extern "C" int scsiTapeCommand()
{
    image_config_t &img = *(image_config_t*)scsiDev.target->cfg;
    tape_drive_t *tape_info = g_tape_drive[img.scsiId & S2S_CFG_TARGET_ID_BITS];
    int commandHandled = 1;

    uint8_t command = scsiDev.cdb[0];

    if (command == 0x08)
    {
        // READ6
        bool fixed = scsiDev.cdb[1] & 1;
        bool suppress_invalid_length = scsiDev.cdb[1] & 2;

        if (img.quirks == S2S_CFG_QUIRKS_OMTI)
        {
            fixed = true;
        }

        uint32_t length =
            (((uint32_t) scsiDev.cdb[2]) << 16) +
            (((uint32_t) scsiDev.cdb[3]) << 8) +
            scsiDev.cdb[4];
        if (fixed && suppress_invalid_length)
        {
            scsiDev.status = CHECK_CONDITION;
            scsiDev.target->sense.code = ILLEGAL_REQUEST;
            scsiDev.target->sense.asc = INVALID_FIELD_IN_CDB;
            scsiDev.phase = STATUS;
        }
        else if (length > 0)
        {
            
            // Check if this is a .TAP format file
            if (tape_info->tape_is_tap_format) {
                // For .TAP format, use variable/fixed block reading
                doTapRead(img, length, fixed, suppress_invalid_length);
            } else {
                uint32_t blocklen = scsiDev.target->liveCfg.bytesPerSector;
                // Use existing multi-file tape logic
                bool underlength = (length > blocklen);
                bool overlength = (length < blocklen);
                if (fixed || (suppress_invalid_length && (overlength || !underlength)))
                {
                    if (fixed)
                    {
                        dbgmsg("----- Host set both fixed block length and supress invalid length indicator which is invalid");
                    }
                    else
                    {
                        dbgmsg("------ Host requested variable block max ", (int)length, " bytes, blocksize is ", (int)blocklen);
                    }
                    scsiDev.status = CHECK_CONDITION;
                    scsiDev.target->sense.code = ILLEGAL_REQUEST;
                    scsiDev.target->sense.asc = INVALID_FIELD_IN_CDB;
                    scsiDev.phase = STATUS;
                    return 1;
                }
                doTapeRead(length);
                if (length > 0)
                    scsiDev.target->tapeBOM = 0;
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

        uint32_t blocksize = fixed ? scsiDev.target->liveCfg.bytesPerSector : length;
        uint32_t blocks_to_write = fixed ? length : 1;

        if (fixed)
            dbgmsg("Tape write blocks: ", (int)length, " blocksize ", (int) blocksize, ", fixed length blocks");
        else
            dbgmsg("Tape write length: ", (int)length, " bytes, variable length blocks");

        if (blocks_to_write > 0)
        {
            if (tape_info->tape_length_mb > 0 && (tape_info->data_pos + blocks_to_write * blocksize) / 1024 / 1024 > tape_info->tape_length_mb)
            {
                scsiDev.status = CHECK_CONDITION;
                scsiDev.target->sense.code = ILLEGAL_REQUEST;
                scsiDev.target->sense.asc = VOLUME_OVERFLOW;
                scsiDev.target->sense.info = length;
                scsiDev.phase = STATUS;
                return 1;
            }

            // Check if this is a .TAP format file
            if (tape_info->tape_is_tap_format) {
                // Start .TAP record write
                tapMediumStartWrite(length, fixed);
            } else {
                // Use existing block-based write
                scsiDiskStartWrite(tape_info->file_pos, blocks_to_write);
                tape_info->file_pos += blocks_to_write;
                scsiDev.target->tapeBOM = 0;
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

            if (tape_info->tape_is_tap_format)
            {
                doTapVerify(length, fixed);
            }
            else
            {
                tape_info->file_pos += length;
                scsiDev.target->tapeBOM = 0;
                scsiDev.status = GOOD;
                scsiDev.phase = STATUS;
            }
        }
    }
    else if (command == 0x19)
    {
        // Erase
        if (tape_info->tape_is_tap_format)
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
                else if (img.file.truncate(tape_info->file_pos))
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
                    scsiDev.target->tapeBOM = 0;
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
            tape_info->file_pos = img.scsiSectors;
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
        uint32_t max_block_size = scsiTapeMaxSectorSize();
        uint32_t min_block_size = scsiTapeMinSectorSize();
        scsiDev.data[0] = 0; // Reserved
        scsiDev.data[1] = (max_block_size >> 16) & 0xFF; // Maximum block length (MSB)
        scsiDev.data[2] = (max_block_size >>  8) & 0xFF;
        scsiDev.data[3] = (max_block_size      ) & 0xFF; // Maximum block length (LSB)
        scsiDev.data[4] = (min_block_size >>  8) & 0xFF; // Minimum block length (MSB)
        scsiDev.data[5] = (min_block_size      ) & 0xFF; // Minimum block length (LSB)
        scsiDev.dataLen = 6;
        scsiDev.phase = DATA_IN;
    }
    else if (command == 0x10)
    {
        // WRITE FILEMARKS
        if (tape_info->tape_is_tap_format) {
            // Number of filemarks to write
            uint32_t count = ((uint32_t) scsiDev.cdb[2]) << 16
                        |    ((uint32_t)scsiDev.cdb[3]) << 8
                        |    scsiDev.cdb[4];

            for (uint32_t i = 0; i < count; i++) {
                tap_result_t result = tapWriteFilemark(img);
                if (result != TAP_OK) {
                    scsiDev.status = CHECK_CONDITION;
                    scsiDev.target->sense.code = MEDIUM_ERROR;
                    scsiDev.target->sense.asc = NO_ADDITIONAL_SENSE_INFORMATION;
                    scsiDev.target->sense.info = count - i;
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
        // signed 24bit number
        int32_t count =
            (((int32_t) scsiDev.cdb[2]) << 24) +
            (((int32_t) scsiDev.cdb[3]) << 16) +
            (((int32_t) scsiDev.cdb[4]) << 8);
        count = count >> 8 ;// arithmetic shift right, preserves the sign
        dbgmsg("------ SPACE count is ", (int) count, " type is ", code);
        if (tape_info->tape_is_tap_format) {
            // Handle .TAP format spacing
            tap_result_t result = TAP_OK;
            uint32_t actual;
            bool set_sense_info = true;
            if (code == 0) {
                // Space blocks

                if (scsiDev.target->liveCfg.bytesPerSector == 0)
                {
                    // Block Space unsupported with variable length blocks
                    scsiDev.status = CHECK_CONDITION;
                    scsiDev.target->sense.code = ILLEGAL_REQUEST;
                    scsiDev.target->sense.asc = NO_ADDITIONAL_SENSE_INFORMATION;
                    scsiDev.phase = STATUS;
                    return 1;
                }

                result = count >= 0 ? tapSpaceForward(img, actual, count, false) : tapSpaceBackward(img, actual, -count, false) ;
            } else if (code == 1) {
                // Space filemarks
                result = count >= 0 ? tapSpaceForward(img, actual, count, true) : tapSpaceBackward(img, actual, -count, true);
            } else if (code == 3) {
                // Space to end of data - move to end of file
                result = tapSpaceEndOfData(img);
                set_sense_info = false;
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
                scsiDev.status = MEDIUM_ERROR;
                scsiDev.target->sense.code = BLANK_CHECK;
                scsiDev.target->sense.asc = NO_ADDITIONAL_SENSE_INFORMATION;
                if (set_sense_info)
                    scsiDev.target->sense.info = count - actual;
                scsiDev.phase = STATUS;
            } else if (result == TAP_BEGINNING_OF_TAPE) {
                scsiDev.target->sense.eom = true;
                scsiDev.status = CHECK_CONDITION;
                scsiDev.target->sense.code = NO_SENSE;
                scsiDev.target->sense.asc = NO_ADDITIONAL_SENSE_INFORMATION;
                if (set_sense_info)
                    scsiDev.target->sense.info = count - actual;
                scsiDev.phase = STATUS;
            } else if (result == TAP_END_OF_DATA) {
                scsiDev.status = CHECK_CONDITION;
                scsiDev.target->sense.code = BLANK_CHECK;
                scsiDev.target->sense.asc = NO_ADDITIONAL_SENSE_INFORMATION;
                if (set_sense_info)
                    scsiDev.target->sense.info = count - actual;
                scsiDev.phase = STATUS;
            } else if (result == TAP_FILEMARK) {
                scsiDev.target->sense.filemark = true;
                scsiDev.status = CHECK_CONDITION;
                scsiDev.target->sense.code = NO_SENSE;
                scsiDev.target->sense.asc = NO_ADDITIONAL_SENSE_INFORMATION;
                if (set_sense_info)
                    scsiDev.target->sense.info = count - actual;
                scsiDev.phase = STATUS;
            } else if (result == TAP_ERROR) {
                scsiDev.status = CHECK_CONDITION;
                scsiDev.target->sense.code = MEDIUM_ERROR;
                scsiDev.target->sense.asc = NO_ADDITIONAL_SENSE_INFORMATION;
                if (set_sense_info)
                    scsiDev.target->sense.info = count - actual;
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
                    tape_info->file_pos = count;
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
    else if (command == 0x1B)
    {
        // START STOP UNIT
        bool loej = (scsiDev.cdb[4] & 0x02) != 0;
        bool start = (scsiDev.cdb[4] & 0x01) != 0;

        dbgmsg("------ StartStopUnit tape LoEj=", loej ? 1 : 0,
               " Start=", start ? 1 : 0,
               " file_pos=", (int)tape_info->file_pos);
        // For tape: Start=1,LoEj=1 means load/retension, Start=0,LoEj=1 means unload.
        // Compatibility: emulate load/retension by rewinding to BOT.
        if (loej && start)
        {
            doRewind();
            dbgmsg("------ Tape load/retension emulated as rewind to BOT");
        }
        // Unload is a no-op in emulation.
        scsiDev.status = GOOD;
        scsiDev.phase = STATUS;
    }
    else if (command == 0x2B)
    {
        // Locate 10 / Seek 10
        if (!tape_info->tape_is_tap_format)
        {
            // Locate is not supported by non simh tap format tape files
            return 0;
        }

        // specific tape drives may calculate address differently, currently treating is as SCSI LBA
        // bool block_type_device = !!(scsiDev.cdb[1] & 4); // true - device specific value (assume SCSI LBA), false - SCSI LBA.

        if (scsiDev.cdb[1] & 2)
        {
            // BT CP (bit 2) is 1 and not supported
            logmsg("Change partition flag not supported for Locate command");
            scsiDev.status = CHECK_CONDITION;
            scsiDev.target->sense.code = ILLEGAL_REQUEST;
            scsiDev.target->sense.asc = INVALID_FIELD_IN_CDB;
            scsiDev.phase = STATUS;
        }
        else
        {
            uint32_t lba =
                (((uint32_t) scsiDev.cdb[3]) << 24) +
                (((uint32_t) scsiDev.cdb[4]) << 16) +
                (((uint32_t) scsiDev.cdb[5]) << 8) +
                scsiDev.cdb[6];

            doLocate(lba);
        }
    }
    else if (command == 0x34)
    {
        // Read Position
        uint32_t bytesPerSector = scsiDev.target->liveCfg.bytesPerSector;
        if (bytesPerSector > 0)
        {
            uint32_t lba = tape_info->data_pos / bytesPerSector;
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
            // Variable sector size set, Read Position not supported
            scsiDev.status = CHECK_CONDITION;
            scsiDev.target->sense.code = ILLEGAL_REQUEST;
            scsiDev.target->sense.asc = NO_ADDITIONAL_SENSE_INFORMATION;
            scsiDev.phase = STATUS;
        }
    }
    else
    {
        commandHandled = 0;
    }

    return commandHandled;
}
