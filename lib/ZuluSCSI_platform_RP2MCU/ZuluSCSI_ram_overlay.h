/**
 * ZuluSCSI RAM Overlay System
 *
 * Provides a write-back cache for disk blocks, enabling the Mac
 * to modify the disk image without immediate SD writes while
 * keeping changes visible to the Akai via SCSI.
 *
 * Copyright (c) 2025
 * Licensed under GPL-3.0
 */

#ifndef ZULUSCSI_RAM_OVERLAY_H
#define ZULUSCSI_RAM_OVERLAY_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Configuration
// =============================================================================

// Maximum blocks in overlay (256 blocks = 128KB)
#ifndef RAM_OVERLAY_CAPACITY
#define RAM_OVERLAY_CAPACITY    256
#endif

#define RAM_OVERLAY_BLOCK_SIZE  512

// =============================================================================
// Overlay Entry Flags
// =============================================================================

#define OVERLAY_FLAG_VALID      (1 << 0)    // Entry contains valid data
#define OVERLAY_FLAG_DIRTY      (1 << 1)    // Modified, needs sync to SD
#define OVERLAY_FLAG_FROM_MAC   (1 << 2)    // Written by Mac via USB
#define OVERLAY_FLAG_FROM_AKAI  (1 << 3)    // Written by Akai via SCSI

// =============================================================================
// Source Identifiers
// =============================================================================

typedef enum {
    OVERLAY_SOURCE_MAC  = OVERLAY_FLAG_FROM_MAC,
    OVERLAY_SOURCE_AKAI = OVERLAY_FLAG_FROM_AKAI,
} overlay_source_t;

// =============================================================================
// Statistics
// =============================================================================

typedef struct {
    uint16_t capacity;          // Max entries
    uint16_t used;              // Current entries
    uint16_t mac_blocks;        // Blocks from Mac
    uint16_t akai_blocks;       // Blocks from Akai
    uint16_t dirty_blocks;      // Blocks needing sync
    uint32_t total_reads;       // Total read operations
    uint32_t total_writes;      // Total write operations
    uint32_t cache_hits;        // Reads served from overlay
    uint32_t cache_misses;      // Reads requiring SD access
    uint32_t evictions;         // Blocks evicted
} overlay_stats_t;

// =============================================================================
// API Functions
// =============================================================================

/**
 * Initialize the RAM overlay system.
 * Call once during startup.
 */
void ram_overlay_init(void);

/**
 * Attempt to read a block from the overlay.
 *
 * @param lba       Logical block address to read
 * @param buffer    Output buffer (must be at least 512 bytes)
 * @return          true if block was found in overlay, false if not cached
 */
bool ram_overlay_read(uint32_t lba, uint8_t* buffer);

/**
 * Write a block to the overlay.
 *
 * @param lba       Logical block address to write
 * @param buffer    Data to write (512 bytes)
 * @param source    Source of the write (OVERLAY_SOURCE_MAC or OVERLAY_SOURCE_AKAI)
 * @return          true if written successfully, false if overlay is full
 */
bool ram_overlay_write(uint32_t lba, const uint8_t* buffer, overlay_source_t source);

/**
 * Check if a specific block is in the overlay.
 *
 * @param lba       Logical block address to check
 * @return          true if block is cached
 */
bool ram_overlay_has_block(uint32_t lba);

/**
 * Check if a specific block is dirty (needs sync).
 *
 * @param lba       Logical block address to check
 * @return          true if block is dirty
 */
bool ram_overlay_is_dirty(uint32_t lba);

/**
 * Invalidate (remove) a specific block from the overlay.
 *
 * @param lba       Logical block address to invalidate
 * @return          true if block was removed, false if not found
 */
bool ram_overlay_invalidate(uint32_t lba);

/**
 * Clear all entries from the overlay.
 * Does NOT sync to SD first - caller should sync if needed.
 */
void ram_overlay_clear(void);

/**
 * Get list of dirty block LBAs.
 *
 * @param filter    Filter type (0=all, 1=MAC only, 2=AKAI only)
 * @param lba_list  Output array for LBAs
 * @param max_count Maximum entries to return
 * @return          Number of LBAs written to list
 */
uint16_t ram_overlay_get_dirty_list(uint8_t filter, uint32_t* lba_list, uint16_t max_count);

/**
 * Get total count of dirty blocks.
 *
 * @param filter    Filter type (0=all, 1=MAC only, 2=AKAI only)
 * @return          Number of dirty blocks matching filter
 */
uint16_t ram_overlay_get_dirty_count(uint8_t filter);

/**
 * Sync all dirty blocks to SD card.
 *
 * @param clear_after   If true, clear overlay after successful sync
 * @param verify        If true, verify writes by reading back
 * @param blocks_synced Output: number of blocks written
 * @param errors        Output: number of errors encountered
 * @return              0 on success, 1 on partial success, 2 on failure
 */
uint8_t ram_overlay_sync_to_sd(bool clear_after, bool verify,
                               uint16_t* blocks_synced, uint16_t* errors);

/**
 * Mark a block as clean (synced to SD).
 * Used after Akai writes are forwarded to SD.
 *
 * @param lba       Logical block address
 */
void ram_overlay_mark_clean(uint32_t lba);

/**
 * Get overlay statistics.
 *
 * @param stats     Output structure for statistics
 */
void ram_overlay_get_stats(overlay_stats_t* stats);

/**
 * Get number of blocks currently in overlay.
 */
uint16_t ram_overlay_get_used(void);

/**
 * Get capacity of overlay (max blocks).
 */
uint16_t ram_overlay_get_capacity(void);

/**
 * Check if overlay is at capacity.
 */
bool ram_overlay_is_full(void);

/**
 * Get a pointer to block data in overlay (for zero-copy reads).
 * Returns NULL if block not in overlay.
 *
 * @param lba       Logical block address
 * @return          Pointer to 512-byte block data, or NULL
 */
const uint8_t* ram_overlay_get_block_ptr(uint32_t lba);

#ifdef __cplusplus
}
#endif

#endif // ZULUSCSI_RAM_OVERLAY_H
