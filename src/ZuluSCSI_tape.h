// Tape device emulation
// Will be called by scsi.c from SCSI2SD.

#pragma once

#include <stdint.h>

// Forward declaration
struct image_config_t;

// SIMH .TAP format constants
#define TAP_MARKER_TAPEMARK    0x00000000  // Filemark marker
#define TAP_MARKER_ERASE_GAP   0xFFFFFFFE  // Erase gap marker
#define TAP_MARKER_END_MEDIUM  0xFFFFFFFF  // End of medium marker

// .TAP record structure for parsing
struct tap_record_t {
    uint32_t length;        // Record length (0 = filemark)
    uint8_t record_class;   // SIMH record class (bits 31-28)
    bool is_filemark;       // True if this is a filemark
    bool is_eom;           // True if end of medium
    bool is_error;         // True if read/parse error occurred
};

// .TAP navigation result
enum tap_result_t {
    TAP_OK = 0,
    TAP_END_OF_TAPE,
    TAP_BEGINNING_OF_TAPE,
    TAP_FILEMARK,
    TAP_ERROR
};

// Helper functions for .TAP format
bool tapIsTapFormat(image_config_t &img);
tap_result_t tapReadRecordForward(image_config_t &img, tap_record_t &record, uint8_t *buffer, uint32_t buffer_size);
tap_result_t tapReadRecordBackward(image_config_t &img, tap_record_t &record, uint8_t *buffer, uint32_t buffer_size);
tap_result_t tapWriteRecord(image_config_t &img, const uint8_t *data, uint32_t length);
tap_result_t tapWriteFilemark(image_config_t &img);
tap_result_t tapWriteEOM(image_config_t &img);
tap_result_t tapWriteEraseGap(image_config_t &img);
tap_result_t tapSpaceForward(image_config_t &img, uint32_t count, bool filemarks);
tap_result_t tapSpaceBackward(image_config_t &img, uint32_t count, bool filemarks);
tap_result_t tapRewind(image_config_t &img);

// .TAP format SCSI write operations
void tapeMediumStartWrite(uint32_t length, bool fixed);
void tapeDataOut();

extern "C" int scsiTapeCommand();