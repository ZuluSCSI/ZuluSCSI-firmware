// Tape device emulation
// Will be called by scsi.c from SCSI2SD.

#pragma once

#include <stdint.h>
#include <scsi.h>

// Forward declaration
struct image_config_t;

// SIMH .TAP format constants
#define TAP_MARKER_TAPEMARK    0x00000000  // Filemark marker
#define TAP_MARKER_ERASE_GAP   0xFFFFFFFE  // Erase gap marker
#define TAP_MARKER_END_MEDIUM  0xFFFFFFFF  // End of medium marker

#define TAPE_TAP_BLOCK_SIZE_MAX (SCSI2SD_BUFFER_SIZE / 2) // max value 0xFFFFFF
#define TAPE_TAP_BLOCK_SIZE_MIN 64 // max value 0xFFFF

#define TAPE_BLOCK_SIZE_MAX (MAX_SECTOR_SIZE) // max value 0xFFFFFF
#define TAPE_BLOCK_SIZE_MIN 64 // max value 0xFFFF


// .TAP record structure for parsing
struct tap_record_t {
    uint64_t leading_length_file_pos;
    uint64_t trailing_length_file_pos;
    uint32_t length;        // Record length (0 = filemark)
    uint8_t record_class;   // SIMH record class (bits 31-28)
    bool is_filemark;       // True if this is a filemark
    bool is_eom;           // True if end of medium
    bool is_error;         // True if read/parse error occurred
};

struct tape_drive_t
{
    // For tape drive emulation
    uint64_t tape_pos; // current position in bytes
    uint32_t tape_length_mb;
    uint32_t tape_mark_index; // a direct relationship to the file in a multi image file tape 
    uint32_t tape_mark_count; // the number of marks
    uint32_t tape_mark_block_offset; // Sum of the the previous image file sizes at the current mark
    bool     tape_load_next_file;
    bool     tape_is_tap_format; // Use SIMH TAP format for storing tape data

};

// .TAP navigation result
enum tap_result_t {
    TAP_OK = 0,
    TAP_END_OF_TAPE,
    TAP_BEGINNING_OF_TAPE,
    TAP_FILEMARK,
    TAP_ERROR
};

// Initialize a Tape device
void tapeInit(uint8_t scsi_id);
// Deinitialize a Tape device
void tapeDeinit(uint8_t scsi_id = 0xFF);

// Set the tape mark count for device with SCSI Id scsi_id 
void setTapeMarkCount(uint8_t scsi_id, uint32_t mark_count);

// Set the whether the Tape device is backed via SIMH file format
void tapeSetIsTap(uint8_t scsi_id, bool is_tap);

// Return true if tape device is a SIMH tap image
bool tapeIsTap();



// Helper functions for .TAP format
tap_result_t tapReadRecordForward(image_config_t &img, tap_record_t &record, uint8_t *buffer, uint32_t buffer_size);
tap_result_t tapReadRecordBackward(image_config_t &img, tap_record_t &record, uint8_t *buffer, uint32_t buffer_size);
tap_result_t tapWriteRecord(image_config_t &img, const uint8_t *data, uint32_t length);
tap_result_t tapWriteFilemark(image_config_t &img);
tap_result_t tapWriteEOM(image_config_t &img);
tap_result_t tapWriteEraseGap(image_config_t &img);
tap_result_t tapSpaceForward(image_config_t &img, uint32_t& actual, uint32_t count, bool filemarks);
tap_result_t tapSpaceBackward(image_config_t &img, uint32_t& actual, uint32_t count, bool filemarks);
tap_result_t tapRewind(image_config_t &img);

// .TAP format SCSI write operations
void tapMediumStartWrite(uint32_t length, bool fixed);
void tapeTapDataOut();
void tapeDataOut();

int scsiTapeMaxSectorSize();
int scsiTapeMinSectorSize();

extern "C" int scsiTapeCommand();