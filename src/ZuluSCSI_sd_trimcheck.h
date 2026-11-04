// SD card erase (TRIM) capability check, run from the USB serial console.
//
// Prints what the card reports about erasing (erase granularity, allocation
// unit, discard/FULE support, the SCR "data after erase" bit when the SD
// driver can read it) and measures what actually happens: it creates a
// temporary file, fills it with a pattern, erases whole erase units inside
// it with the card's ERASE command, and reports how long that took and what
// the erased blocks read back as (zeros, ones or unchanged).
//
// The SCSI bus is not serviced while the check runs. Only the temporary
// file is affected.

#pragma once

#include <stdint.h>
#include <stdbool.h>

void sdTrimCheckRun();

// The measurement behind the check, for callers that need its verdict.
typedef struct
{
    uint32_t unit_sectors;   // erase unit tested, in 512-byte sectors (the card's allocation unit)
    uint32_t worst_erase_ms; // slowest erase of one unit, rounded up
    bool reads_zero;         // every erased unit read back as zeros
    bool neighbour_ok;       // the unit after the erased ones was left alone
    bool scr_known;          // the SD driver could read the card's SCR register
    bool scr_zero;           // ... and it says that erased blocks read back as zeros
} sd_erase_measure_t;

// Runs the test with a temporary file; returns false if it could not be
// completed (no room, erase refused, ...). Needs scsiDev.data as scratch space,
// so the SCSI bus must not be in the middle of a transfer. With verbose set it
// prints what the console command prints.
bool sdTrimMeasure(sd_erase_measure_t *out, bool verbose);
