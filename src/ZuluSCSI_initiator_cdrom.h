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

// Initiator mode imaging of CD-ROM drives into .bin/.cue files

#pragma once

#include <stdint.h>

class FsFile;

enum initiator_cd_image_t {
    INITIATOR_CD_IMAGE_ISO,    // Single data track, image as .iso
    INITIATOR_CD_IMAGE_BINCUE, // Audio or several tracks, image as .bin/.cue
    INITIATOR_CD_IMAGE_SKIP    // Disc cannot be imaged with this drive
};

enum initiator_cd_step_t {
    INITIATOR_CD_CONTINUE,
    INITIATOR_CD_DONE,
    INITIATOR_CD_FAILED
};

// Read the table of contents and probe how the drive can read the tracks.
// vendor is the INQUIRY vendor identification, used for vendor commands.
initiator_cd_image_t scsiInitiatorCDProbe(int target_id, const char *vendor);

// Number of sectors that will be read, valid after probe
uint32_t scsiInitiatorCDTotalSectors();

// Size of the .bin file, valid after probe
uint64_t scsiInitiatorCDTotalBytes();

// Start imaging into the .bin file. The .cue sheet is written next to it
// with the same name when imaging ends.
bool scsiInitiatorCDBegin(FsFile *file, const char *bin_filename, uint8_t max_retry);

// Read the next sectors. sectors_done is the progress in sectors,
// sectors_in_batch the number of sectors handled in this step.
initiator_cd_step_t scsiInitiatorCDStep(uint32_t *sectors_done, uint32_t *sectors_in_batch);

// Write the .cue sheet, log the summary and restore the drive block size
void scsiInitiatorCDEnd();
