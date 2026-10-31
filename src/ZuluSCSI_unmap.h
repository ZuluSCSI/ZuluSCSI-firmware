// SCSI UNMAP (thin provisioning) for fixed disks that are read directly from
// the SD card. Announced only when [SCSI] SdScrub = Yes and the card's erase
// reads back as zeros (ZuluSCSI_sd_zero.h), and only for a standard disk
// identity: captured AS/400 identities and custom inquiry data stay untouched.
//
// The functions below look at the current target (scsiDev.target) and may use
// scsiDev.data as scratch space, so call them before filling it.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Non-zero if the current target announces UNMAP and honours it. Pass non-zero
// for standardIdentity if the caller already knows the target answers with the
// built-in standard inquiry data (so no custom inquiry data is checked for).
int scsiDiskUnmapAdvertised(int standardIdentity);

// Builds the Block Limits (0xB0) or Logical Block Provisioning (0xB2) VPD page
// into buf and returns its length, or 0 if pageCode is not one of those or the
// target does not announce UNMAP. buf may be scsiDev.data.
uint32_t scsiDiskUnmapVpdPage(uint8_t pageCode, uint8_t *buf);

#ifdef __cplusplus
}
#endif
