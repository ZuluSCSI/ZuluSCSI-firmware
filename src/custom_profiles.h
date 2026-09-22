/**
 * ZuluSCSI™ - Copyright (c) 2025 Rabbit Hole Computing™
 *
 * ZuluSCSI™ firmware is licensed under the GPL version 3 or any later version.
 *
 * https://www.gnu.org/licenses/gpl-3.0.html
 * ----
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

// Splitting a multi-section .ini definitions file into one standalone
// profile file per [Section].

#pragma once

#include <ZuluSCSI_platform_config.h>

#if defined(PLATFORM_AS400) && defined(ZULUSCSI_MCU_RP23XX)

// Split a multi-section definitions file into one .ini file per [Section]
// under CUSTOM_PROFILES_DIR/, so each profile is also available as a
// standalone, self-contained profile file. "[59H7001]" and its keys become
// zulu_profiles/59H7001.ini holding that one section. Existing files of the
// same name are overwritten, so this stays in step with the definitions file
// on every boot.
//
// The split itself is generic -- it copies whatever sections and key/value
// pairs it finds. AS400_PROFILES_FILE is simply the definitions file the
// firmware ships with today; nothing here depends on its contents being
// AS/400 drive captures.
//
// Call once the SD card is known to be mounted; a missing or empty
// definitions file is not an error and returns quietly. Not reentrant: it
// keeps its browse state in a static, so never call it from two contexts at
// once.
void splitCustomProfileDefinitions();

#endif // PLATFORM_AS400 && ZULUSCSI_MCU_RP23XX
