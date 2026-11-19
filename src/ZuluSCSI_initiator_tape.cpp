/**
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

/*
 * Initiator mode imaging of SCSI tape drives.
 *
 * The tape is read from the beginning until the drive reports the end of
 * recorded data. Every block read from the tape becomes one record in a
 * SIMH .tap file and every filemark becomes a tape mark:
 *
 *   record:   <length LE32> <data> [pad byte if length is odd] <length LE32>
 *   filemark: 0x00000000
 *   end:      0xFFFFFFFF (only when the drive reports the physical end)
 *
 * Records that the drive could not read are flagged with bit 31 of both
 * length words (SIMH class 8, bad data record).
 *
 * Tapes can be read in fixed block mode (e.g. QIC-150, which always uses
 * 512 byte blocks) or variable block mode (e.g. DDS). In variable mode the
 * drive returns each block as it was written, the length is calculated from
 * the residue reported in the sense data.
 *
 * Reference: https://simh.trailing-edge.com/docs/simh_magtape.pdf
 */

#include "ZuluSCSI_initiator_tape.h"
#include "ZuluSCSI_initiator.h"
#include "ZuluSCSI_config.h"
#include "ZuluSCSI_log.h"
#include <ZuluSCSI_platform.h>
#include <minIni.h>
#include "SdFat.h"

#include <scsi2sd.h>
extern "C" {
#include <scsi.h>
}

#ifdef PLATFORM_HAS_INITIATOR_MODE

extern SdFs SD;

// Not in the SCSI_MESSAGE enum in scsi.h
#define MSG_NO_OPERATION 0x08

#define TAP_RECORD_BAD 0x80000000
#define TAP_MARKER_TAPEMARK 0x00000000
#define TAP_MARKER_END_MEDIUM 0xFFFFFFFF

// Tape operations can take a long time before the drive responds.
// The watchdog is kept alive for this long while waiting.
#define TAPE_READ_TIMEOUT_MS   (180 * 1000)
#define TAPE_REWIND_TIMEOUT_MS (300 * 1000)
#define TAPE_SPACE_TIMEOUT_MS  (180 * 1000)

// Data is received in chunks so that the drive waiting in the middle of a
// transfer does not trigger the 10 second timeout of the PHY read routine.
#define TAPE_READ_CHUNK 8192

// Flush the image file after this many bytes have been written
#define TAPE_FLUSH_INTERVAL (1024 * 1024)

// Interval for progress messages in the log
#define TAPE_PROGRESS_INTERVAL_MS 5000

// Number of UNIT ATTENTION and ABORTED COMMAND retries for a single read
#define TAPE_MAX_TRANSIENT_RETRIES 4

// Transfer length field in READ(6) is 24 bits
#define TAPE_MAX_TRANSFER_LENGTH 0xFFFFFF

struct tape_sense_t {
    uint8_t key;
    uint8_t asc;
    uint8_t ascq;
    bool filemark;
    bool eom;
    bool ili;
    bool valid; // Information field is valid
    int32_t info;
};

static struct {
    int target_id;
    FsFile *file;
    uint8_t max_errors;

    // Block size in fixed mode, 0 for variable mode
    uint32_t block_size;

    // Largest record that fits in the buffer, limited by READ BLOCK LIMITS
    uint32_t max_record;

    // Transfer length requested in variable mode. Requesting exactly the
    // length of the next record avoids waiting for a phase change at the
    // end of each short transfer, so the last record length is used.
    uint32_t request_len;

    // Set when the previous read is retried after spacing back one block
    bool reread;

    uint8_t consecutive_errors;
    uint8_t transient_retries;

    // Statistics
    uint32_t file_number;
    uint32_t records_in_file;
    uint32_t records;
    uint32_t filemarks;
    uint32_t bad_records;
    uint32_t truncated_records;
    uint64_t bytes;
    uint64_t bytes_in_file;
    uint32_t last_record_len;

    uint64_t bytes_since_flush;
    uint32_t start_time;
    uint32_t last_progress_time;
    uint64_t last_progress_bytes;
} g_tape;

/*************************************
 * Low level commands                *
 *************************************/

static void writeLE32(uint8_t *data, uint32_t value)
{
    data[0] = value & 0xFF;
    data[1] = (value >> 8) & 0xFF;
    data[2] = (value >> 16) & 0xFF;
    data[3] = (value >> 24) & 0xFF;
}

// Execute REQUEST SENSE and parse the tape specific fields
static bool tapeRequestSense(int target_id, tape_sense_t *sense)
{
    uint8_t command[6] = {0x03, 0, 0, 0, 18, 0};
    uint8_t response[18] = {0};

    // Some drives return less than 18 bytes, which makes the command
    // report a failure. Use the response anyway if it looks valid.
    scsiInitiatorRunCommand(target_id, command, sizeof(command),
                            response, sizeof(response), NULL, 0);

    memset(sense, 0, sizeof(*sense));
    if ((response[0] & 0x7E) != 0x70)
    {
        logmsg("-- Tape REQUEST SENSE returned unexpected data: ", bytearray(response, sizeof(response)));
        sense->key = 0xFF;
        return false;
    }

    sense->valid = !!(response[0] & 0x80);
    sense->filemark = !!(response[2] & 0x80);
    sense->eom = !!(response[2] & 0x40);
    sense->ili = !!(response[2] & 0x20);
    sense->key = response[2] & 0x0F;
    sense->info = (int32_t)(((uint32_t)response[3] << 24) |
                            ((uint32_t)response[4] << 16) |
                            ((uint32_t)response[5] << 8) |
                            ((uint32_t)response[6]));
    sense->asc = response[12];
    sense->ascq = response[13];

    dbgmsg("-- Tape sense key ", sense->key, " asc ", sense->asc, " ascq ", sense->ascq,
           " FM ", (int)sense->filemark, " EOM ", (int)sense->eom, " ILI ", (int)sense->ili,
           " info ", (int)sense->info, sense->valid ? "" : " (not valid)");
    return true;
}

// Execute a command that does not transfer data, e.g. REWIND or SPACE.
// If retry_unit_attention is set, the command is repeated after a pending
// UNIT ATTENTION, e.g. the one reported after MODE SELECT.
static bool tapeSimpleCommand(int target_id, const uint8_t *command, const char *name, uint32_t timeout,
                              bool retry_unit_attention = false)
{
    for (int attempt = 0; attempt < 3; attempt++)
    {
        int status = scsiInitiatorRunCommand(target_id, command, 6, NULL, 0, NULL, 0, false, timeout);
        if (status == 0)
        {
            return true;
        }

        if (status != 2)
        {
            logmsg("-- Tape ", name, " failed, status ", status);
            return false;
        }

        tape_sense_t sense;
        tapeRequestSense(target_id, &sense);
        if (retry_unit_attention && sense.key == UNIT_ATTENTION)
        {
            dbgmsg("-- Tape ", name, ": UNIT ATTENTION asc ", sense.asc, ", retrying");
            continue;
        }

        logmsg("-- Tape ", name, " failed, sense key ", sense.key,
               " asc ", sense.asc, " ascq ", sense.ascq);
        return false;
    }

    logmsg("-- Tape ", name, " failed, drive keeps reporting UNIT ATTENTION");
    return false;
}

// Execute a READ command and receive up to len bytes.
// Unlike scsiInitiatorRunCommand(), a DATA_IN phase that ends before len
// bytes have been received is not an error: in variable block mode this is
// how the drive returns a block that is shorter than requested.
// Returns the SCSI status, or a negative value if the transfer failed.
static int tapeReadCommand(int target_id, const uint8_t *command, uint8_t *buf, uint32_t len, uint32_t *received)
{
    *received = 0;

    int status = scsiInitiatorRunCommand(target_id, command, 6, NULL, 0, NULL, 0,
                                         true, TAPE_READ_TIMEOUT_MS);
    if (status != 0)
    {
        // Selection failed, or the command completed without a data phase
        return status;
    }

    // Status 0 is returned both when the drive enters the DATA_IN phase and
    // when the command completed with GOOD status without a data phase.
    // In the DATA_IN phase REQ stays asserted until we acknowledge the byte.
    SCSI_PHASE phase;
    uint32_t wait_start = millis();
    while ((phase = (SCSI_PHASE)scsiHostPhyGetPhase()) == BUS_BUSY &&
           (uint32_t)(millis() - wait_start) < 100)
    {
    }

    if (phase != DATA_IN)
    {
        return 0;
    }

    int busWidth = scsiInitiatorGetBusWidth(target_id);
    bool transfer_ok = true;
    bool overrun = false;
    status = -1;

    uint32_t last_activity = millis();
    while ((phase = (SCSI_PHASE)scsiHostPhyGetPhase()) != BUS_FREE)
    {
        platform_poll();

        if ((uint32_t)(millis() - last_activity) < TAPE_READ_TIMEOUT_MS)
        {
            platform_reset_watchdog();
        }

        if (phase == BUS_BUSY)
        {
            // Waiting for the drive to assert REQ
            continue;
        }

        last_activity = millis();

        if (phase == DATA_IN)
        {
            if (*received >= len)
            {
                // Should not happen, the drive never returns more than requested
                uint8_t dummy;
                scsiHostRead(&dummy, 1);
                overrun = true;
                continue;
            }

            uint32_t chunk = len - *received;
            if (chunk > TAPE_READ_CHUNK) chunk = TAPE_READ_CHUNK;

            scsiHostSetBusWidth(busWidth);
            uint32_t count = scsiHostRead(buf + *received, chunk);
            scsiHostSetBusWidth(0);

            if (count == 0)
            {
                // Parity error or bus reset
                logmsg("-- Tape data transfer failed after ", (int)*received, " bytes");
                transfer_ok = false;
                break;
            }

            *received += count;
        }
        else if (phase == STATUS)
        {
            uint8_t tmp = 0xFF;
            scsiHostRead(&tmp, 1);
            status = tmp;
        }
        else if (phase == MESSAGE_IN)
        {
            uint8_t msg = 0;
            scsiHostRead(&msg, 1);
            if (msg == MSG_COMMAND_COMPLETE)
            {
                break;
            }
        }
        else if (phase == MESSAGE_OUT)
        {
            uint8_t msg = MSG_NO_OPERATION;
            scsiHostWrite(&msg, 1);
        }
        else
        {
            logmsg("-- Tape READ: unexpected phase ", (int)phase);
            transfer_ok = false;
            break;
        }
    }

    scsiHostWaitBusFree();

    if (overrun)
    {
        logmsg("-- Tape READ: drive sent more than the requested ", (int)len, " bytes");
    }

    if (!transfer_ok || status < 0)
    {
        // Transfer failed or the drive released the bus without status
        return -2;
    }

    return status;
}

/*************************************
 * SIMH .tap output                  *
 *************************************/

static bool tapCheckSize(uint32_t len)
{
    if (SD.fatType() == FAT_TYPE_EXFAT)
    {
        return true;
    }

    uint64_t end = g_tape.file->curPosition() + (uint64_t)len + 9;
    if (end > 0xFFFFFFFFULL)
    {
        logmsg("-- Tape image reached the 4 GiB file size limit of FAT32");
        logmsg("-- Please reformat the SD card with exFAT format to image this tape fully");
        return false;
    }
    return true;
}

static bool tapWriteWord(uint32_t value)
{
    uint8_t buf[4];
    writeLE32(buf, value);
    return g_tape.file->write(buf, 4) == 4;
}

static bool tapWriteRecord(const uint8_t *data, uint32_t len, bool bad)
{
    if (len == 0)
    {
        // A zero length word would be read back as a tape mark
        dbgmsg("-- Tape: skipping zero length block at record ", (int)g_tape.records);
        if (bad) g_tape.bad_records++;
        return true;
    }

    if (!tapCheckSize(len))
    {
        return false;
    }

    uint32_t word = len | (bad ? TAP_RECORD_BAD : 0);
    bool ok = tapWriteWord(word);
    ok = ok && (g_tape.file->write(data, len) == len);
    if (ok && (len & 1))
    {
        // SIMH pads records to an even length
        uint8_t pad = 0;
        ok = (g_tape.file->write(&pad, 1) == 1);
    }
    ok = ok && tapWriteWord(word);

    if (!ok)
    {
        logmsg("-- Tape image: SD card write failed");
        return false;
    }

    if (len != g_tape.last_record_len)
    {
        if (g_tape.records == 0 || g_tape.records_in_file == 0)
        {
            logmsg("-- Tape file ", (int)g_tape.file_number, " record size ", (int)len, " bytes");
        }
        else
        {
            dbgmsg("-- Tape record size changed to ", (int)len, " bytes at record ", (int)g_tape.records);
        }
        g_tape.last_record_len = len;
    }

    g_tape.records++;
    g_tape.records_in_file++;
    g_tape.bytes += len;
    g_tape.bytes_in_file += len;
    g_tape.bytes_since_flush += len + 8;
    if (bad) g_tape.bad_records++;

    if (g_tape.bytes_since_flush >= TAPE_FLUSH_INTERVAL)
    {
        g_tape.file->flush();
        g_tape.bytes_since_flush = 0;
    }

    return true;
}

static bool tapWriteFilemark()
{
    if (!tapCheckSize(0) || !tapWriteWord(TAP_MARKER_TAPEMARK))
    {
        logmsg("-- Tape image: SD card write failed");
        return false;
    }

    logmsg("-- Tape file ", (int)g_tape.file_number, ": ", (int)g_tape.records_in_file,
           " records, ", (int)(g_tape.bytes_in_file / 1024), " KiB");

    g_tape.filemarks++;
    g_tape.file_number++;
    g_tape.records_in_file = 0;
    g_tape.bytes_in_file = 0;
    g_tape.last_record_len = 0;
    g_tape.file->flush();
    g_tape.bytes_since_flush = 0;
    return true;
}

// Write fixed size blocks as individual records
static bool tapWriteBlocks(const uint8_t *data, uint32_t len)
{
    uint32_t count = len / g_tape.block_size;
    for (uint32_t i = 0; i < count; i++)
    {
        if (!tapWriteRecord(data + i * g_tape.block_size, g_tape.block_size, false))
        {
            return false;
        }
    }

    if (len % g_tape.block_size != 0)
    {
        logmsg("-- Tape READ returned ", (int)len, " bytes, not a multiple of the block size ",
               (int)g_tape.block_size);
    }
    return true;
}

/*************************************
 * Imaging state machine             *
 *************************************/

static void tapeUpdateLed()
{
    // The size of the tape is not known, so instead of a progress bar
    // the LED blinks at a constant rate while copying.
    if ((millis() % 250) < 125)
    {
        LED_ON();
    }
    else
    {
        LED_OFF();
    }
}

static void tapeLogProgress(bool force)
{
    uint32_t now = millis();
    uint32_t elapsed = now - g_tape.last_progress_time;
    if (!force && elapsed < TAPE_PROGRESS_INTERVAL_MS)
    {
        return;
    }

    int speed_kbps = 0;
    if (elapsed > 0)
    {
        speed_kbps = (int)((g_tape.bytes - g_tape.last_progress_bytes) / elapsed);
    }

    logmsg("Tape read: ", (int)(g_tape.bytes / (1024 * 1024)), " MiB, file ", (int)g_tape.file_number,
           ", ", (int)g_tape.records, " records, speed ", speed_kbps, " kB/s");

    g_tape.last_progress_time = now;
    g_tape.last_progress_bytes = g_tape.bytes;
}

// Space back over the block that was just read, so that it can be read
// again with the correct transfer length.
static bool tapeSpaceBackOneBlock()
{
    uint8_t command[6] = {0x11, 0x00, 0xFF, 0xFF, 0xFF, 0x00}; // SPACE -1 blocks
    return tapeSimpleCommand(g_tape.target_id, command, "SPACE", TAPE_SPACE_TIMEOUT_MS);
}

// Handle a read error that could not be recovered by the drive.
// Returns false if the error limit was reached.
static bool tapeCountError()
{
    g_tape.consecutive_errors++;
    if (g_tape.consecutive_errors > g_tape.max_errors)
    {
        logmsg("-- Tape: ", (int)g_tape.consecutive_errors, " consecutive read errors, giving up");
        return false;
    }
    return true;
}

// Write the data of a variable block read that ended with ILI set
static bool tapeHandleVariableLength(const tape_sense_t &sense, uint32_t received, bool *retry)
{
    *retry = false;
    uint32_t requested = g_tape.request_len;

    if (!sense.valid)
    {
        // Length of the block is unknown, assume it was what we received
        g_tape.request_len = g_tape.max_record;
        return tapWriteRecord(scsiDev.data, received, false);
    }

    int64_t actual = (int64_t)requested - sense.info;
    if (actual < 0) actual = 0;

    if (actual > requested)
    {
        // Block is longer than requested. The drive transferred the start of
        // the block and moved past it, read it again with a larger length.
        if (!g_tape.reread && actual <= g_tape.max_record && tapeSpaceBackOneBlock())
        {
            dbgmsg("-- Tape block of ", (int)actual, " bytes is longer than requested ",
                   (int)requested, ", reading it again");
            g_tape.request_len = (uint32_t)actual;
            g_tape.reread = true;
            *retry = true;
            return true;
        }

        logmsg("-- Tape block at record ", (int)g_tape.records, " is ", (int)actual,
               " bytes, only the first ", (int)received, " bytes could be read");
        if (actual > g_tape.max_record)
        {
            logmsg("-- The largest block supported is ", (int)g_tape.max_record, " bytes");
        }
        g_tape.truncated_records++;
        g_tape.request_len = g_tape.max_record;
        return tapWriteRecord(scsiDev.data, received, true);
    }

    if ((uint32_t)actual != received)
    {
        // The data phase ended early or the last byte of an odd length block
        // was lost. Read the block again requesting its exact length.
        if (!g_tape.reread && tapeSpaceBackOneBlock())
        {
            dbgmsg("-- Tape block of ", (int)actual, " bytes, received ", (int)received,
                   ", reading it again");
            g_tape.request_len = (uint32_t)actual;
            g_tape.reread = true;
            *retry = true;
            return true;
        }

        logmsg("-- Tape block at record ", (int)g_tape.records, " is ", (int)actual,
               " bytes, received ", (int)received);
        g_tape.request_len = g_tape.max_record;
        return tapWriteRecord(scsiDev.data, received, true);
    }

    // Shorter block that was received completely
    g_tape.request_len = (actual > 0) ? (uint32_t)actual : g_tape.max_record;
    return tapWriteRecord(scsiDev.data, received, false);
}

bool scsiInitiatorTapeBegin(int target_id, FsFile *file, uint8_t max_errors)
{
    memset(&g_tape, 0, sizeof(g_tape));
    g_tape.target_id = target_id;
    g_tape.file = file;
    g_tape.max_errors = max_errors;

    uint32_t buffer_size = sizeof(scsiDev.data);
    if (buffer_size > TAPE_MAX_TRANSFER_LENGTH) buffer_size = TAPE_MAX_TRANSFER_LENGTH;

    // Supported block sizes
    uint8_t limits_cmd[6] = {0x05, 0, 0, 0, 0, 0};
    uint8_t limits[6] = {0};
    bool limits_ok = scsiInitiatorRunCommand(target_id, limits_cmd, sizeof(limits_cmd),
                                             limits, sizeof(limits), NULL, 0) == 0;
    uint32_t max_block = ((uint32_t)limits[1] << 16) | ((uint32_t)limits[2] << 8) | limits[3];
    uint32_t min_block = ((uint32_t)limits[4] << 8) | limits[5];
    if (limits_ok)
    {
        logmsg("-- Tape block limits: min ", (int)min_block, " max ", (int)max_block, " bytes");
    }
    else
    {
        tape_sense_t sense;
        tapeRequestSense(target_id, &sense);
        logmsg("-- Tape READ BLOCK LIMITS not supported");
    }

    // Current density and block size
    uint8_t sense_cmd[6] = {0x1A, 0, 0, 0, 12, 0};
    uint8_t mode[12] = {0};
    int mode_status = scsiInitiatorRunCommand(target_id, sense_cmd, sizeof(sense_cmd),
                                              mode, sizeof(mode), NULL, 0);
    bool has_descriptor = (mode[3] >= 8);
    uint8_t density = has_descriptor ? mode[4] : 0;
    uint32_t current_block = has_descriptor ?
        (((uint32_t)mode[9] << 16) | ((uint32_t)mode[10] << 8) | mode[11]) : 0;
    if (mode_status == 2)
    {
        tape_sense_t sense;
        tapeRequestSense(target_id, &sense);
    }
    if (has_descriptor)
    {
        logmsg("-- Tape density code ", density, ", block size ",
               current_block ? (int)current_block : 0, current_block ? " bytes" : " (variable)",
               (mode[2] & 0x80) ? ", write protected" : "");
    }

    // Select block mode
    long forced = ini_getl("SCSI", "InitiatorTapeBlockSize", -1, CONFIGFILE);
    if (forced >= 0)
    {
        g_tape.block_size = (uint32_t)forced;
        logmsg("-- InitiatorTapeBlockSize set to ", (int)forced, " in ", CONFIGFILE);
    }
    else if (limits_ok && min_block != 0 && min_block == max_block)
    {
        // Drive supports only one block size, e.g. QIC-150
        g_tape.block_size = min_block;
    }
    else
    {
        // Use variable mode if possible, it reads tapes written in any block size
        g_tape.block_size = 0;
    }

    if ((forced >= 0 || g_tape.block_size == 0) &&
        (!has_descriptor || current_block != g_tape.block_size))
    {
        // Keep the current density and buffered mode, change the block size
        uint8_t params[12] = {0, 0, has_descriptor ? (uint8_t)(mode[2] & 0x7F) : (uint8_t)0x10, 8,
            density, 0, 0, 0,
            0, (uint8_t)(g_tape.block_size >> 16), (uint8_t)(g_tape.block_size >> 8), (uint8_t)g_tape.block_size
        };

        // Try first without and then with the page format bit,
        // drives differ in which one they accept.
        bool selected = false;
        for (uint8_t pf = 0; pf <= 0x10 && !selected; pf += 0x10)
        {
            uint8_t select_cmd[6] = {0x15, pf, 0, 0, sizeof(params), 0};
            int status = scsiInitiatorRunCommand(target_id, select_cmd, sizeof(select_cmd),
                                                 NULL, 0, params, sizeof(params));
            if (status == 2)
            {
                tape_sense_t sense;
                tapeRequestSense(target_id, &sense);
            }
            selected = (status == 0);
        }

        if (!selected)
        {
            logmsg("-- Tape MODE SELECT failed, using the current block size of the drive");
            g_tape.block_size = current_block;
        }
    }

    g_tape.max_record = buffer_size;
    if (g_tape.block_size == 0 && limits_ok && max_block != 0 && max_block < g_tape.max_record)
    {
        g_tape.max_record = max_block;
    }
    g_tape.request_len = g_tape.max_record;

    if (g_tape.block_size > buffer_size)
    {
        logmsg("-- Tape block size ", (int)g_tape.block_size, " is larger than the buffer of ",
               (int)buffer_size, " bytes, cannot image this tape");
        return false;
    }

    if (g_tape.block_size == 0)
    {
        logmsg("-- Reading tape in variable block mode, largest block ", (int)g_tape.max_record, " bytes");
    }
    else
    {
        logmsg("-- Reading tape in fixed block mode, ", (int)g_tape.block_size, " bytes per block");
    }

    if (SD.fatType() != FAT_TYPE_EXFAT)
    {
        logmsg("-- Note: on FAT32 the tape image is limited to 4 GiB");
    }

    logmsg("-- Rewinding tape");
    uint8_t rewind_cmd[6] = {0x01, 0, 0, 0, 0, 0};
    if (!tapeSimpleCommand(target_id, rewind_cmd, "REWIND", TAPE_REWIND_TIMEOUT_MS, true))
    {
        return false;
    }

    g_tape.start_time = millis();
    g_tape.last_progress_time = g_tape.start_time;
    return true;
}

initiator_tape_step_t scsiInitiatorTapeStep()
{
    tapeUpdateLed();
    tapeLogProgress(false);

    bool fixed = (g_tape.block_size != 0);
    uint32_t len;
    uint8_t command[6] = {0x08, 0, 0, 0, 0, 0};

    if (fixed)
    {
        uint32_t blocks = sizeof(scsiDev.data) / g_tape.block_size;
        if (blocks > TAPE_MAX_TRANSFER_LENGTH) blocks = TAPE_MAX_TRANSFER_LENGTH;
        len = blocks * g_tape.block_size;
        command[1] = 0x01; // FIXED
        command[2] = (uint8_t)(blocks >> 16);
        command[3] = (uint8_t)(blocks >> 8);
        command[4] = (uint8_t)blocks;
    }
    else
    {
        len = g_tape.request_len;
        command[2] = (uint8_t)(len >> 16);
        command[3] = (uint8_t)(len >> 8);
        command[4] = (uint8_t)len;
    }

    uint32_t received = 0;
    int status = tapeReadCommand(g_tape.target_id, command, scsiDev.data, len, &received);
    bool reread = g_tape.reread;
    g_tape.reread = false;

    if (status == -1)
    {
        logmsg("-- Tape drive stopped responding");
        return INITIATOR_TAPE_FAILED;
    }
    else if (status < 0)
    {
        // Transfer failed, the data is lost but the tape has moved on
        logmsg("-- Tape READ transfer failed at record ", (int)g_tape.records);
        tape_sense_t sense;
        tapeRequestSense(g_tape.target_id, &sense);
        return tapeCountError() ? INITIATOR_TAPE_CONTINUE : INITIATOR_TAPE_FAILED;
    }
    else if (status == 0)
    {
        bool ok;
        if (fixed)
        {
            ok = tapWriteBlocks(scsiDev.data, received);
        }
        else
        {
            if (received != len)
            {
                logmsg("-- Tape READ completed with ", (int)received, " of ", (int)len, " bytes");
            }
            ok = tapWriteRecord(scsiDev.data, received, false);
        }
        g_tape.consecutive_errors = 0;
        g_tape.transient_retries = 0;
        return ok ? INITIATOR_TAPE_CONTINUE : INITIATOR_TAPE_FAILED;
    }
    else if (status != 2)
    {
        logmsg("-- Tape READ returned unexpected status ", status);
        return tapeCountError() ? INITIATOR_TAPE_CONTINUE : INITIATOR_TAPE_FAILED;
    }

    // CHECK CONDITION
    tape_sense_t sense;
    if (!tapeRequestSense(g_tape.target_id, &sense))
    {
        return tapeCountError() ? INITIATOR_TAPE_CONTINUE : INITIATOR_TAPE_FAILED;
    }

    // END-OF-DATA DETECTED and END-OF-DATA NOT FOUND (blank tape) mark the
    // end of the recorded data on some drives that do not report BLANK CHECK
    bool end_of_data = (sense.key == BLANK_CHECK) ||
                       (sense.asc == 0x00 && sense.ascq == 0x05) ||
                       (sense.asc == 0x14 && sense.ascq == 0x03);

    if (sense.key == UNIT_ATTENTION)
    {
        if (sense.asc == 0x28 || sense.asc == 0x29)
        {
            // Medium changed or reset, the tape position is lost
            logmsg("-- Tape drive reported UNIT ATTENTION asc ", sense.asc,
                   ", the tape position has been lost");
            return INITIATOR_TAPE_FAILED;
        }

        if (++g_tape.transient_retries > TAPE_MAX_TRANSIENT_RETRIES)
        {
            logmsg("-- Tape drive keeps reporting UNIT ATTENTION asc ", sense.asc);
            return INITIATOR_TAPE_FAILED;
        }
        g_tape.reread = reread;
        return INITIATOR_TAPE_CONTINUE;
    }

    if (sense.key == ABORTED_COMMAND && received == 0)
    {
        if (++g_tape.transient_retries > TAPE_MAX_TRANSIENT_RETRIES)
        {
            logmsg("-- Tape READ aborted repeatedly");
            return INITIATOR_TAPE_FAILED;
        }
        logmsg("-- Tape READ aborted by drive, retrying");
        g_tape.reread = reread;
        return INITIATOR_TAPE_CONTINUE;
    }
    g_tape.transient_retries = 0;

    if (sense.key == NO_SENSE || sense.key == RECOVERED_ERROR || end_of_data)
    {
        // Data transferred before the condition
        if (fixed)
        {
            if (!tapWriteBlocks(scsiDev.data, received)) return INITIATOR_TAPE_FAILED;

            if (sense.ili && !sense.filemark && !end_of_data && !sense.eom)
            {
                // Block on tape does not match the block size
                logmsg("-- Tape block at record ", (int)g_tape.records,
                       " does not match the block size ", (int)g_tape.block_size);
                logmsg("-- Set InitiatorTapeBlockSize = 0 in ", CONFIGFILE, " to read it in variable block mode");
                memset(scsiDev.data, 0, g_tape.block_size);
                if (!tapWriteRecord(scsiDev.data, g_tape.block_size, true)) return INITIATOR_TAPE_FAILED;
                return tapeCountError() ? INITIATOR_TAPE_CONTINUE : INITIATOR_TAPE_FAILED;
            }
        }
        else if (sense.ili)
        {
            bool retry;
            if (!tapeHandleVariableLength(sense, received, &retry)) return INITIATOR_TAPE_FAILED;
            if (retry) return INITIATOR_TAPE_CONTINUE;
        }
        else if (received > 0)
        {
            if (!tapWriteRecord(scsiDev.data, received, false)) return INITIATOR_TAPE_FAILED;
        }

        g_tape.consecutive_errors = 0;

        if (sense.filemark)
        {
            if (!tapWriteFilemark()) return INITIATOR_TAPE_FAILED;
        }

        if (end_of_data)
        {
            logmsg("-- Reached end of recorded data on tape");
            return INITIATOR_TAPE_DONE;
        }

        if (sense.eom)
        {
            logmsg("-- Reached physical end of tape");
            tapWriteWord(TAP_MARKER_END_MEDIUM);
            return INITIATOR_TAPE_DONE;
        }

        return INITIATOR_TAPE_CONTINUE;
    }

    if (sense.key == MEDIUM_ERROR)
    {
        logmsg("-- Tape read error at record ", (int)g_tape.records,
               ", asc ", sense.asc, " ascq ", sense.ascq);

        if (fixed)
        {
            // Keep the blocks before the bad one and mark the bad block
            uint32_t good = received - received % g_tape.block_size;
            if (sense.valid && sense.info > 0)
            {
                uint32_t blocks = len / g_tape.block_size;
                uint32_t missing = (uint32_t)sense.info;
                if (missing <= blocks)
                {
                    uint32_t transferred = (blocks - missing) * g_tape.block_size;
                    if (transferred < good) good = transferred;
                }
            }
            if (!tapWriteBlocks(scsiDev.data, good)) return INITIATOR_TAPE_FAILED;

            uint32_t partial = received - good;
            if (partial > g_tape.block_size) partial = g_tape.block_size;
            memmove(scsiDev.data, scsiDev.data + good, partial);
            memset(scsiDev.data + partial, 0, g_tape.block_size - partial);
            if (!tapWriteRecord(scsiDev.data, g_tape.block_size, true)) return INITIATOR_TAPE_FAILED;
        }
        else if (received > 0)
        {
            if (!tapWriteRecord(scsiDev.data, received, true)) return INITIATOR_TAPE_FAILED;
        }
        else
        {
            g_tape.bad_records++;
        }

        if (sense.filemark && !tapWriteFilemark()) return INITIATOR_TAPE_FAILED;

        if (sense.eom)
        {
            logmsg("-- Reached physical end of tape");
            tapWriteWord(TAP_MARKER_END_MEDIUM);
            return INITIATOR_TAPE_DONE;
        }

        g_tape.request_len = g_tape.max_record;
        return tapeCountError() ? INITIATOR_TAPE_CONTINUE : INITIATOR_TAPE_FAILED;
    }

    logmsg("-- Tape READ failed, sense key ", sense.key, " asc ", sense.asc, " ascq ", sense.ascq);
    return INITIATOR_TAPE_FAILED;
}

void scsiInitiatorTapeEnd()
{
    if (g_tape.records_in_file > 0)
    {
        logmsg("-- Tape file ", (int)g_tape.file_number, ": ", (int)g_tape.records_in_file,
               " records, ", (int)(g_tape.bytes_in_file / 1024), " KiB (no filemark at end)");
    }

    g_tape.file->flush();
    tapeLogProgress(true);

    uint32_t elapsed_s = (millis() - g_tape.start_time) / 1000;
    logmsg("Tape image: ", (int)(g_tape.bytes / 1024), " KiB in ", (int)g_tape.records,
           " records and ", (int)g_tape.filemarks, " filemarks, ", (int)elapsed_s, " seconds");

    if (g_tape.bad_records > 0)
    {
        logmsg("NOTE: ", (int)g_tape.bad_records, " tape blocks could not be read");
    }
    if (g_tape.truncated_records > 0)
    {
        logmsg("NOTE: ", (int)g_tape.truncated_records, " tape blocks were too large and were truncated");
    }

    logmsg("-- Rewinding tape");
    uint8_t rewind_cmd[6] = {0x01, 0, 0, 0, 0, 0};
    tapeSimpleCommand(g_tape.target_id, rewind_cmd, "REWIND", TAPE_REWIND_TIMEOUT_MS, true);
    LED_OFF();
}

bool scsiInitiatorTapeMediumChanged(int target_id)
{
    uint8_t command[6] = {0x00, 0, 0, 0, 0, 0}; // TEST UNIT READY
    int status = scsiInitiatorRunCommand(target_id, command, sizeof(command), NULL, 0, NULL, 0);
    if (status != 2)
    {
        return false;
    }

    tape_sense_t sense;
    tapeRequestSense(target_id, &sense);
    return sense.key == UNIT_ATTENTION;
}

#endif
