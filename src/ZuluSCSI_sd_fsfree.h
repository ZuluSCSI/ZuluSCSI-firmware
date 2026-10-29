// Free space of the SD card's own file system (the one that holds the image
// files), for the SD scrubber (ZuluSCSI_sd_scrub.h): clusters that no file
// uses hold nothing worth keeping, whatever they contain, so whole erase units
// made of free clusters can be released with ERASE without reading them first.
// That also covers space freed by deleting or truncating files, which is not
// zeros.
//
// This is only safe while nobody but ZuluSCSI changes the file system. A host
// that is given direct access to the card (raw mode, partition passthrough, or
// the whole card when no image was found) can allocate clusters behind
// ZuluSCSI's back, so nothing here reports free space while any target does.
//
// Supported: exFAT and FAT32. Both are read directly from the card, after the
// file system library's caches were written out, so call these only from the
// main loop with nothing else touching the file system in between.

#pragma once

#include <stdint.h>

// Checks that the card's file system can be consulted: a supported type, no
// target has direct access to the card, and the free-space bookkeeping on the
// card agrees with what the file system library itself says (see the
// implementation). Takes a few milliseconds up to a fraction of a second, so
// call it when the SCSI bus is quiet. Logs why if it returns false.
bool sdFsFreeStart();

// True only if every cluster that overlaps SD sectors [firstSector,
// firstSector + sectors) is free and the range lies inside the file system's
// cluster area. Also false if anything is uncertain (read error, a target with
// direct access to the card, sdFsFreeStart() not successful). Uses
// scsiDev.data as scratch space.
bool sdFsRangeFree(uint32_t firstSector, uint32_t sectors);

// Forget everything, e.g. when the card was changed.
void sdFsFreeReset();
