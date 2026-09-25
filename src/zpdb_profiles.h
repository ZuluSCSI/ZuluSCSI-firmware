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

// Drive profiles in flash: boot-time ingest from the SD card, and the
// per-target binding that makes a lookup cheap at INQUIRY time.
//
// Profiles come from two ZPDB stores, searched in this order:
//
//   1. the custom store, in the flash profile region. At boot
//      zpdbProfilesInit() looks in /zulu_profiles for .ini files --
//      custom_profiles.cpp splits the card's definitions file out there:
//
//        - no .ini files: the store already in flash is used as-is
//        - one or more:   the flash region is erased and rebuilt from them,
//                         then each file is moved into /zulu_profiles/loaded
//                         or /zulu_profiles/failed depending on how it went
//
//   2. the built-in store, compiled into the firmware image from the
//      `profile_definitions` file named in platformio.ini (see
//      src/generate-embedded-profiles.py)
//
// So a profile in the custom overrides a built-in one of the same name, and a
// card needs no profile files at all to use a built-in profile.
//
// A profile is bound to a SCSI ID once (zpdbBindProfile), which resolves and
// caches its section and the store it is in. Serving a page after that reads
// the value straight out of flash into the destination buffer -- normally
// scsiDev.data -- so no per-target copy of the profile data is held in RAM.

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <scsi2sd.h>
#include "zpdb.h"

// Directory on the SD card holding profile .ini files, and the two
// subdirectories finished files are moved into.
#ifndef ZPDB_PROFILE_DIR
#define ZPDB_PROFILE_DIR "/zulu_profiles"
#endif
#define ZPDB_LOADED_DIR ZPDB_PROFILE_DIR "/loaded"
#define ZPDB_FAILED_DIR ZPDB_PROFILE_DIR "/failed"

// Longest .ini line this reader accepts. Deliberately larger than the lines
// it expects: the longest capture in tree today is 435 characters, and a
// profile that arrives inside as400_disk_definitions.txt cannot exceed 512
// anyway, because custom_profiles.cpp splits that file with minIni's
// ini_browse(), whose INI_BUFFERSIZE is 512 and holds the section name, key
// name and value in one buffer. The headroom is so that a hand-written file
// with an over-long line is reported rather than silently truncated.
// Most sections a rebuild will accept. Only used to spot a duplicate profile
// name across files while writing, when there is no finished store to search
// yet: 4 bytes per section.
#ifndef ZPDB_MAX_PROFILES
#define ZPDB_MAX_PROFILES 96
#endif

#ifndef ZPDB_INI_LINE_MAX
#define ZPDB_INI_LINE_MAX 1024
#endif

// Scratch one ingest needs: the writer's three page buffers, a line buffer,
// room to decode one line of hex, and the per-profile name hashes. The store
// does not own a buffer of its own -- see zpdbProfilesInit().
#define ZPDB_REBUILD_SCRATCH_SIZE (3u * ZPDB_MAX_SECTION_SIZE                                         + ZPDB_INI_LINE_MAX                                                + 1024u                                                            + ZPDB_MAX_PROFILES * sizeof(uint32_t))

// Open whatever store is in flash and, when `scratch` is given, first ingest
// any .ini files waiting in /zulu_profiles. Also opens the built-in store.
//
// Opening is unconditional -- both stores live in flash and survive the card
// being pulled. Ingesting is the part that needs memory, and the caller
// supplies it: pass a buffer of at least ZPDB_REBUILD_SCRATCH_SIZE bytes,
// 4-byte aligned, that is free for the duration of the call. Passing nullptr
// (or a buffer that is too small) opens the store without ingesting.
//
// The store deliberately has no opinion about where that memory comes from.
// In this firmware the caller lends it scsiDev.data, which is only free of
// host data on the power-on path, before scsiInit() has run -- so a card
// inserted later, or a ROM-drive boot that never had one, passes nullptr and
// simply opens what flash holds. Drop the .ini files in and power-cycle to
// ingest them.
void zpdbProfilesInit(uint8_t *scratch, size_t scratch_size);

// True when either store is open and can be queried.
bool zpdbProfilesAvailable();

// Number of profiles across both open stores. A name present in both is
// counted twice, although only the custom store copy is ever bound.
uint32_t zpdbProfileCount();

// Forget every binding. Call alongside resetCustomInquiryData().
void zpdbUnbindAll();

// Resolve `profileName` to a section -- custom store first, then the
// built-in one -- and remember it for this SCSI ID. Returns false (and logs)
// when neither store has such a profile.
bool zpdbBindProfile(uint8_t scsiId, const char *profileName);

// True when this SCSI ID has a profile bound.
bool zpdbHasProfile(uint8_t scsiId);

// Name of the profile bound to this SCSI ID, or nullptr.
const char *zpdbProfileName(uint8_t scsiId);

// Read one page of the bound profile straight into `buf`. These do no
// injection -- the caller patches identity fields in place afterwards, in
// the buffer it is about to serve from.
//
// *length is set to the number of bytes read. Returns false if there is no
// binding, no such key, or the value does not fit `buf_size`.
bool zpdbReadVPD(uint8_t scsiId, uint8_t pageCode, uint8_t *buf, uint32_t buf_size,
                 uint32_t *length);
bool zpdbReadSPD(uint8_t scsiId, uint8_t *buf, uint32_t buf_size, uint32_t *length);
bool zpdbReadModeSense(uint8_t scsiId, uint8_t *buf, uint32_t buf_size, uint32_t *length);
bool zpdbReadLogSense(uint8_t scsiId, uint8_t pageCode, uint8_t *buf, uint32_t buf_size,
                      uint32_t *length);

// BlockSize / Sectors from the bound profile, when it declares them.
bool zpdbReadCapacity(uint8_t scsiId, uint32_t *blockSize, uint64_t *sectors);

// BlockSize / Sectors of a profile named directly, binding nothing, resolved
// in the same store order as zpdbBindProfile(). For the
// dynamic SCSI ID, whose image has to be sized and created before the ID it
// will answer to is known -- binding is per SCSI ID, and the ID only arrives
// once the SCA backplane can be read.
bool zpdbReadCapacityByName(const char *profileName, uint32_t *blockSize, uint64_t *sectors);

// Does the bound profile carry this VPD page at all? Used to decide whether
// a page is supported without reading it.
bool zpdbHasVPD(uint8_t scsiId, uint8_t pageCode);
