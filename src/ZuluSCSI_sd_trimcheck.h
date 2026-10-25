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

void sdTrimCheckRun();
