/**
 * ZuluSCSI™ - Copyright (c) 2026 Rabbit Hole Computing™
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

// USB serial console "Erase (TRIM) SD card" submenu.
//
// Destroys the entire content of the SD card via the card's own ERASE
// command (CMD32/CMD33/CMD38) -- not a filesystem-level operation, this
// erases every sector regardless of what filesystem or partitions exist.
// Two-stage confirmation: the platform's normal "press key, then 'y' to
// engage" mechanism (see serial_menu() in ZuluSCSI_platform.cpp) is stage
// one; entering this submenu is stage two, requiring the user to type the
// literal word TRIM (case-sensitive) before anything is touched.
//
// Runs the erase as a single blocking call once confirmed -- consistent
// with this console's existing destructive commands (reboot, exposing the
// SD card as a USB drive, etc.), which already fully interrupt normal SCSI
// service when invoked. The SCSI bus will not be serviced for the
// duration of the erase.

#pragma once

// Returns true while the erase submenu is active (either waiting for the
// typed confirmation, or -- briefly, since the erase itself is blocking --
// while actually erasing). The platform serial_menu() checks this to
// route incoming characters here instead of processing them as top-level
// console commands.
bool serialEraseMenuActive();

// Feed one character received from the USB serial port into the submenu.
// Only called when serialEraseMenuActive() returns true.
void serialEraseMenuProcess(char c);

// Enter the erase submenu. Called by serial_menu() after the user
// confirms the erase command with 'y'. Displays the warning and the
// "type TRIM to confirm" prompt; does not touch the SD card yet.
void serialEraseMenuEnter();
