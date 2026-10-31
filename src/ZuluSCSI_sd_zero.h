// Zeroing of image sectors with the SD card's erase, instead of writing zeros.
//
// A host that clears a large range (SCSI WRITE SAME with a zero block) would
// otherwise make ZuluSCSI program every zero into the card's flash. When the
// card reads erased blocks back as zeros -- which is measured, see
// ZuluSCSI_sd_trimcheck.h -- whole erase units inside the range can be
// released with the card's ERASE command, which the card's controller can
// use to recycle flash. The host sees exactly the same data either way.
//
// Off unless [SCSI] SdTrim = Yes is set in zuluscsi.ini.

#pragma once

#include <stdint.h>
#include "ImageBackingStore.h"

// True if the card is known to read erased blocks back as zeros (and SdTrim or
// SdScrub is on). Measured with a temporary file the first time it is asked, which takes a
// few seconds; the result is kept until sdEraseGateReset(). If it returns true,
// *unitSectors (when given) is the card's erase unit in 512-byte sectors.
bool sdEraseReadsAsZero(uint32_t *unitSectors = nullptr);

// Forget the measurement, e.g. after the card was changed.
void sdEraseGateReset();

// Slowest erase of one unit seen while measuring, in milliseconds (0 if not measured).
uint32_t sdEraseWorstMs();

// Writes zeros to count logical sectors of the image, starting at lba. Whole
// erase units inside the range are released with the card's erase when
// sdEraseReadsAsZero() allows it and the image is read directly from the card;
// everything else is written as zeros. Uses scsiDev.data as scratch space.
// Returns false if writing failed. *releasedSectors, if given, is how many
// 512-byte SD sectors were released instead of written.
bool sdZeroSectors(ImageBackingStore &file, uint32_t bytesPerSector, uint32_t lba, uint32_t count, uint32_t *releasedSectors = nullptr);

// Slowest measured erase of one unit that still allows using the card's erase
// for UNMAP and for the background scrubber, in milliseconds: an erase cannot
// be interrupted and keeps the next host command waiting, and one UNMAP
// command may release several units back to back.
#define SD_ERASE_MAX_MS 100

// True if UNMAP can be announced and honoured: [SCSI] SdScrub = Yes, the card
// reads erased blocks back as zeros and erases a unit fast enough, as measured
// by sdTrimStart() at boot and after a card change (it never starts a
// measurement itself, as that needs a temporary file on the card). If it returns true, *unitSectors (when given) is the
// card's erase unit in 512-byte sectors.
bool sdUnmapAvailable(uint32_t *unitSectors = nullptr);

// Releases the whole erase units inside count logical sectors from lba with
// the card's erase; the ranges in front of and behind them are left alone, as
// a host that sends UNMAP does not expect any particular content for them.
// Does nothing if the image is not read directly from the card, or if erasing
// failed. *releasedSectors, if given, is how many 512-byte SD sectors were
// released.
void sdUnmapSectors(ImageBackingStore &file, uint32_t bytesPerSector, uint32_t lba, uint32_t count, uint32_t *releasedSectors = nullptr);
