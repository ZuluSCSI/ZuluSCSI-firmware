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

// USB serial console media management submenu.
// Provides an interactive text menu for listing, selecting, ejecting and
// inserting images on removable SCSI devices (CD-ROM, Tape, etc.).
// Driven by the platform's usb_input_poll() / serial_menu() machinery.

#pragma once

#ifdef PLATFORM_MASS_STORAGE

// Returns true while the media submenu is active.
// The platform serial_menu() checks this to route incoming characters here
// instead of processing them as top-level console commands.
extern "C" bool serialMediaMenuActive();

// Feed one character received from the USB serial port into the submenu.
// Only called when serialMediaMenuActive() returns true.
extern "C" void serialMediaMenuProcess(char c);

// Enter the media submenu.  Called by serial_menu() after the user confirms
// the 'm' command with 'y'.  Displays the removable-device list and
// transitions the state machine into DEVICE_LIST state.
extern "C" void serialMediaMenuEnter();

#endif // PLATFORM_MASS_STORAGE
