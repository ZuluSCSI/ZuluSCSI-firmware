/**
 * ZuluSCSI™ - Copyright (c) 2022-2026 Rabbit Hole Computing™
 *
 * This file is licensed under the GPL version 3 or any later version.
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

// Interface for SCA option connector peripherals (ZuluSCSI Wide only).
//

// Call this once after platform_late_init() and before readSCSIDeviceConfig().
// The returned ID maps to the [SCSIn] section in zuluscsi.ini and to image
// directories/files named with the prefix character 'n' (e.g., HDn/, CDn.img).

#pragma once
#include <stdint.h>

#ifdef ZULUSCSI_WIDE

// Return whether the ZuluSCSI is using an SCA connector
bool zuluscsi_is_sca();

// Read the latest SCA configuration signals on the bus
bool zuluscsi_sca_hw_update();

// Get the SCSI ID from the last zuluscsi_sca_hw_update()
// Returns 0–15 on success, or -1 if the expander is absent or the read fails.
int8_t zuluscsi_sca_hw_scsi_id();

// Get wether the last zuluscsi_sca_hw_update() data is valid
bool zuluscsi_sca_hw_valid();

// Get the delayed start status from the last zuluscsi_sca_hw_update()
// Should check zuluscsi_sca_hw_valid before reading the value
bool zuluscsi_sca_hw_delayed_start();

// Get the remote start status from the last zuluscsi_sca_hw_update()
// Should check zuluscsi_sca_hw_valid() before reading the value
bool zululscsi_sca_hw_remote_start();



#endif // ZULUSCSI_WIDE
