/**
 * ZuluSCSI RAM Overlay System - Implementation
 *
 * Provides a write-back cache for disk blocks.
 *
 * Copyright (c) 2025
 * Licensed under GPL-3.0
 */

#include "ZuluSCSI_ram_overlay.h"
#include "ZuluSCSI_log.h"
#include <string.h>

// Forward declarations for SD operations (provided by ZuluSCSI)
extern "C" {
    bool scsiDiskReadBlocks(uint32_t lba, uint8_t* buffer, uint32_t count);
    bool scsiDiskWriteBlocks(uint32_t lba, const uint8_t* buffer, uint32_t count);
}

// =============================================================================
// Internal Data Structures
// =============================================================================

// Single overlay entry
typedef struct {
    uint32_t lba;
    uint8_t  flags;
    uint8_t  reserved[3];
    uint8_t  data[RAM_OVERLAY_BLOCK_SIZE];
} overlay_entry_t;

// Overlay state
static struct {
    overlay_entry_t entries[RAM_OVERLAY_CAPACITY];
    uint16_t        used_count;
    overlay_stats_t stats;
    bool            initialized;
} g_overlay;

// =============================================================================
// Helper Functions
// =============================================================================

/**
 * Find entry by LBA.
 * Returns index if found, -1 if not.
 */
static int find_entry(uint32_t lba)
{
    for (int i = 0; i < RAM_OVERLAY_CAPACITY; i++) {
        if ((g_overlay.entries[i].flags & OVERLAY_FLAG_VALID) &&
            g_overlay.entries[i].lba == lba) {
            return i;
        }
    }
    return -1;
}

/**
 * Find a free slot.
 * Returns index if found, -1 if overlay is full.
 */
static int find_free_slot(void)
{
    for (int i = 0; i < RAM_OVERLAY_CAPACITY; i++) {
        if (!(g_overlay.entries[i].flags & OVERLAY_FLAG_VALID)) {
            return i;
        }
    }
    return -1;
}

// =============================================================================
// API Implementation
// =============================================================================

void ram_overlay_init(void)
{
    memset(&g_overlay, 0, sizeof(g_overlay));
    g_overlay.stats.capacity = RAM_OVERLAY_CAPACITY;
    g_overlay.initialized = true;
    logmsg("RAM overlay initialized: capacity=", RAM_OVERLAY_CAPACITY, " blocks");
}

bool ram_overlay_read(uint32_t lba, uint8_t* buffer)
{
    if (!g_overlay.initialized || !buffer) {
        return false;
    }

    g_overlay.stats.total_reads++;

    int idx = find_entry(lba);
    if (idx >= 0) {
        memcpy(buffer, g_overlay.entries[idx].data, RAM_OVERLAY_BLOCK_SIZE);
        g_overlay.stats.cache_hits++;
        return true;
    }

    g_overlay.stats.cache_misses++;
    return false;
}

bool ram_overlay_write(uint32_t lba, const uint8_t* buffer, overlay_source_t source)
{
    if (!g_overlay.initialized || !buffer) {
        return false;
    }

    g_overlay.stats.total_writes++;

    // Check if block already exists
    int idx = find_entry(lba);

    if (idx < 0) {
        // Need a new slot
        idx = find_free_slot();
        if (idx < 0) {
            // Overlay is full
            dbgmsg("RAM overlay full, cannot write LBA ", lba);
            return false;
        }
        g_overlay.used_count++;
        g_overlay.stats.used = g_overlay.used_count;
    } else {
        // Updating existing entry - adjust source counts
        if (g_overlay.entries[idx].flags & OVERLAY_FLAG_FROM_MAC) {
            if (g_overlay.stats.mac_blocks > 0) g_overlay.stats.mac_blocks--;
        }
        if (g_overlay.entries[idx].flags & OVERLAY_FLAG_FROM_AKAI) {
            if (g_overlay.stats.akai_blocks > 0) g_overlay.stats.akai_blocks--;
        }
        if (g_overlay.entries[idx].flags & OVERLAY_FLAG_DIRTY) {
            if (g_overlay.stats.dirty_blocks > 0) g_overlay.stats.dirty_blocks--;
        }
    }

    // Write the entry
    g_overlay.entries[idx].lba = lba;
    g_overlay.entries[idx].flags = OVERLAY_FLAG_VALID | OVERLAY_FLAG_DIRTY | source;
    memcpy(g_overlay.entries[idx].data, buffer, RAM_OVERLAY_BLOCK_SIZE);

    // Update stats
    if (source == OVERLAY_SOURCE_MAC) {
        g_overlay.stats.mac_blocks++;
    } else {
        g_overlay.stats.akai_blocks++;
    }
    g_overlay.stats.dirty_blocks++;

    return true;
}

bool ram_overlay_has_block(uint32_t lba)
{
    return find_entry(lba) >= 0;
}

bool ram_overlay_is_dirty(uint32_t lba)
{
    int idx = find_entry(lba);
    if (idx >= 0) {
        return (g_overlay.entries[idx].flags & OVERLAY_FLAG_DIRTY) != 0;
    }
    return false;
}

bool ram_overlay_invalidate(uint32_t lba)
{
    int idx = find_entry(lba);
    if (idx < 0) {
        return false;
    }

    // Update stats before clearing
    overlay_entry_t* entry = &g_overlay.entries[idx];
    if (entry->flags & OVERLAY_FLAG_FROM_MAC) {
        if (g_overlay.stats.mac_blocks > 0) g_overlay.stats.mac_blocks--;
    }
    if (entry->flags & OVERLAY_FLAG_FROM_AKAI) {
        if (g_overlay.stats.akai_blocks > 0) g_overlay.stats.akai_blocks--;
    }
    if (entry->flags & OVERLAY_FLAG_DIRTY) {
        if (g_overlay.stats.dirty_blocks > 0) g_overlay.stats.dirty_blocks--;
    }

    // Clear entry
    memset(entry, 0, sizeof(overlay_entry_t));
    g_overlay.used_count--;
    g_overlay.stats.used = g_overlay.used_count;
    g_overlay.stats.evictions++;

    return true;
}

void ram_overlay_clear(void)
{
    for (int i = 0; i < RAM_OVERLAY_CAPACITY; i++) {
        g_overlay.entries[i].flags = 0;
    }
    g_overlay.used_count = 0;
    g_overlay.stats.used = 0;
    g_overlay.stats.mac_blocks = 0;
    g_overlay.stats.akai_blocks = 0;
    g_overlay.stats.dirty_blocks = 0;

    logmsg("RAM overlay cleared");
}

uint16_t ram_overlay_get_dirty_list(uint8_t filter, uint32_t* lba_list, uint16_t max_count)
{
    if (!lba_list || max_count == 0) {
        return 0;
    }

    uint16_t count = 0;

    for (int i = 0; i < RAM_OVERLAY_CAPACITY && count < max_count; i++) {
        overlay_entry_t* entry = &g_overlay.entries[i];

        if (!(entry->flags & OVERLAY_FLAG_VALID)) continue;
        if (!(entry->flags & OVERLAY_FLAG_DIRTY)) continue;

        // Apply filter
        if (filter == 1 && !(entry->flags & OVERLAY_FLAG_FROM_MAC)) continue;
        if (filter == 2 && !(entry->flags & OVERLAY_FLAG_FROM_AKAI)) continue;

        lba_list[count++] = entry->lba;
    }

    return count;
}

uint16_t ram_overlay_get_dirty_count(uint8_t filter)
{
    uint16_t count = 0;

    for (int i = 0; i < RAM_OVERLAY_CAPACITY; i++) {
        overlay_entry_t* entry = &g_overlay.entries[i];

        if (!(entry->flags & OVERLAY_FLAG_VALID)) continue;
        if (!(entry->flags & OVERLAY_FLAG_DIRTY)) continue;

        // Apply filter
        if (filter == 1 && !(entry->flags & OVERLAY_FLAG_FROM_MAC)) continue;
        if (filter == 2 && !(entry->flags & OVERLAY_FLAG_FROM_AKAI)) continue;

        count++;
    }

    return count;
}

uint8_t ram_overlay_sync_to_sd(bool clear_after, bool verify,
                                uint16_t* blocks_synced, uint16_t* errors)
{
    uint16_t synced = 0;
    uint16_t err = 0;
    uint8_t verify_buffer[RAM_OVERLAY_BLOCK_SIZE];

    logmsg("Syncing overlay to SD...");

    for (int i = 0; i < RAM_OVERLAY_CAPACITY; i++) {
        overlay_entry_t* entry = &g_overlay.entries[i];

        if (!(entry->flags & OVERLAY_FLAG_VALID)) continue;
        if (!(entry->flags & OVERLAY_FLAG_DIRTY)) continue;

        // Write to SD
        if (scsiDiskWriteBlocks(entry->lba, entry->data, 1)) {
            // Verify if requested
            if (verify) {
                if (scsiDiskReadBlocks(entry->lba, verify_buffer, 1)) {
                    if (memcmp(verify_buffer, entry->data, RAM_OVERLAY_BLOCK_SIZE) != 0) {
                        dbgmsg("Verify failed for LBA ", entry->lba);
                        err++;
                        continue;
                    }
                } else {
                    dbgmsg("Verify read failed for LBA ", entry->lba);
                    err++;
                    continue;
                }
            }

            // Mark as clean
            entry->flags &= ~OVERLAY_FLAG_DIRTY;
            synced++;

            if (g_overlay.stats.dirty_blocks > 0) {
                g_overlay.stats.dirty_blocks--;
            }
        } else {
            dbgmsg("SD write failed for LBA ", entry->lba);
            err++;
        }
    }

    if (clear_after && err == 0) {
        ram_overlay_clear();
    }

    if (blocks_synced) *blocks_synced = synced;
    if (errors) *errors = err;

    logmsg("Sync complete: ", synced, " blocks, ", err, " errors");

    if (err == 0) return 0;                 // Success
    if (synced > 0) return 1;               // Partial success
    return 2;                               // Complete failure
}

void ram_overlay_mark_clean(uint32_t lba)
{
    int idx = find_entry(lba);
    if (idx >= 0 && (g_overlay.entries[idx].flags & OVERLAY_FLAG_DIRTY)) {
        g_overlay.entries[idx].flags &= ~OVERLAY_FLAG_DIRTY;
        if (g_overlay.stats.dirty_blocks > 0) {
            g_overlay.stats.dirty_blocks--;
        }
    }
}

void ram_overlay_get_stats(overlay_stats_t* stats)
{
    if (stats) {
        memcpy(stats, &g_overlay.stats, sizeof(overlay_stats_t));
    }
}

uint16_t ram_overlay_get_used(void)
{
    return g_overlay.used_count;
}

uint16_t ram_overlay_get_capacity(void)
{
    return RAM_OVERLAY_CAPACITY;
}

bool ram_overlay_is_full(void)
{
    return g_overlay.used_count >= RAM_OVERLAY_CAPACITY;
}

const uint8_t* ram_overlay_get_block_ptr(uint32_t lba)
{
    int idx = find_entry(lba);
    if (idx >= 0) {
        return g_overlay.entries[idx].data;
    }
    return nullptr;
}
