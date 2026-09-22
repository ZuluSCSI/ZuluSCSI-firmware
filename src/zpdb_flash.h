/**
 * Copyright (C) 2026 Rabbit Hole Computing LLC
 *
 * This file is part of ZuluSCSI
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
**/

// Flash backing for the ZPDB profile store.
//
// The store lives at a fixed offset in the external flash, between the
// settings area and the ROM drive -- see the flash map in
// ZuluSCSI_platform.h, which is the single place those boundaries are
// written down. There is no partition table involved: the offset is
// compiled in, and the build fails if the map and the linker's idea of how
// much flash the firmware gets ever disagree.
//
// Every offset in this API is store-relative: 0 is the first byte of the
// region. Reads go through platform_flash_read(), which streams the flash
// device directly and never touches the XIP cache or window.

#pragma once

#include <stdint.h>
#include "zpdb.h"

// Locate the store. Safe to call more than once; the checks are done once
// and the result cached. Returns false (having logged why) when the store
// has nowhere to live on this board.
bool zpdbFlashInit();

bool zpdbFlashAvailable();

// Flash-relative byte offset and size of the store, for logging.
uint32_t zpdbFlashBase();
uint32_t zpdbFlashSize();

// zpdb_read_fn: read `len` bytes at store-relative `offset`, bypassing XIP.
// Any offset, length and destination alignment is accepted.
bool zpdbFlashRead(uint32_t offset, void *dest, uint32_t len, void *ctx);

// Erase the whole store. Called before the store is rewritten.
bool zpdbFlashErase();

// ZpdbWriter::program_page_t: program one 4096-byte page at store-relative
// `offset` into the already-erased region.
bool zpdbFlashProgramPage(uint32_t offset, const uint8_t *page, void *ctx);
