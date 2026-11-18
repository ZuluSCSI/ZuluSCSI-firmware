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

// Initiator mode imaging of SCSI tape drives into SIMH .tap files

#pragma once

#include <stdint.h>

class FsFile;

enum initiator_tape_step_t {
    INITIATOR_TAPE_CONTINUE, // More data to read
    INITIATOR_TAPE_DONE,     // Reached end of recorded data
    INITIATOR_TAPE_FAILED    // Imaging stopped because of an error
};

// Rewind the tape and select the block mode used for reading.
// max_errors is the number of consecutive read errors before giving up.
// Returns false if the tape cannot be imaged.
bool scsiInitiatorTapeBegin(int target_id, FsFile *file, uint8_t max_errors);

// Read the next block(s) from tape and append them to the image file
initiator_tape_step_t scsiInitiatorTapeStep();

// Log the summary and rewind the tape
void scsiInitiatorTapeEnd();

// After a tape has been imaged and unloaded, the next tape is detected by
// the UNIT ATTENTION a drive reports when a cartridge is inserted.
// Returns true if the medium has been changed since it was unloaded.
bool scsiInitiatorTapeMediumChanged(int target_id);
