/**
 * ZuluSCSI Disk Overlay Hooks
 *
 * Integration hooks for the RAM overlay system.
 * These functions should be called from ZuluSCSI_disk.cpp.
 *
 * Copyright (c) 2025
 * Licensed under GPL-3.0
 */

#include "ZuluSCSI_ram_overlay.h"
#include "ZuluSCSI_usb_bridge.h"

// =============================================================================
// Integration Hooks - Add to ZuluSCSI_disk.cpp
// =============================================================================

/**
 * Hook for SCSI read operations.
 * Call this BEFORE reading from SD to check overlay first.
 *
 * @param lba       Starting logical block address
 * @param buffer    Buffer to read into
 * @param count     Number of blocks to read
 * @return          Number of blocks served from overlay (0 if all need SD read)
 *
 * Example integration in ZuluSCSI_disk.cpp:
 *
 *   // In scsiDiskRead() or diskDataIn():
 *   for (uint32_t i = 0; i < count; i++) {
 *       if (ram_overlay_read(lba + i, buffer + (i * 512))) {
 *           // Block served from overlay
 *       } else {
 *           // Read from SD card
 *           img->read(offset + (i * 512), buffer + (i * 512), 512);
 *       }
 *   }
 */
extern "C"
uint32_t disk_overlay_read_hook(uint32_t lba, uint8_t* buffer, uint32_t count)
{
    uint32_t overlay_hits = 0;

    for (uint32_t i = 0; i < count; i++) {
        if (ram_overlay_read(lba + i, buffer + (i * 512))) {
            overlay_hits++;
        } else {
            // Caller should read this block from SD
        }
    }

    return overlay_hits;
}

/**
 * Hook for SCSI write operations from Akai.
 * Call this AFTER receiving write data from SCSI.
 *
 * @param lba       Starting logical block address
 * @param buffer    Data written by Akai
 * @param count     Number of blocks written
 *
 * Example integration in ZuluSCSI_disk.cpp:
 *
 *   // In scsiDiskWrite() or diskDataOut():
 *   // After writing to SD, capture in overlay for Mac visibility:
 *   disk_overlay_write_hook(lba, buffer, count);
 */
extern "C"
void disk_overlay_write_hook(uint32_t lba, const uint8_t* buffer, uint32_t count)
{
    // Store Akai writes in overlay for Mac visibility
    for (uint32_t i = 0; i < count; i++) {
        ram_overlay_write(lba + i, buffer + (i * 512), OVERLAY_SOURCE_AKAI);
    }

    // Notify Mac if subscribed
    usb_bridge_send_notify_blocks_written(lba, count);
}

/**
 * Hook for SCSI command notification.
 * Call this when a SCSI command is received.
 *
 * @param command   SCSI command byte
 * @param lba       LBA from command (if applicable)
 * @param count     Block count from command (if applicable)
 */
extern "C"
void disk_scsi_command_hook(uint8_t command, uint32_t lba, uint16_t count)
{
    usb_bridge_send_notify_scsi_command(command, lba, count);
}
