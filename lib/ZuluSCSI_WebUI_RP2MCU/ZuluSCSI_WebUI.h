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

// ZuluSCSI WebUI I2C server - public API.
// Manages communication with the ZuluSCSI-Remote-Interface I2C WebUI client board.
// The I2C WebUI client (address 0x45); ZuluSCSI is I2C master.
// Mirrors the media management API used by the USB serial console.

#pragma once
#ifdef ZULUCONTROL_FIRMWARE

// Read WiFi credentials from zuluscsi.ini [WebUI] section and check for I2C WebUI client.
// Call once after the SD card is mounted and SCSI devices are configured.
void zuluWebUIInit();

// Poll the WebUI I2C interface. Non-blocking when no I2C WebUI client present.
// Call from platform_poll() or the main SCSI loop.
void zuluWebUITask();

// Returns true if the WebUI I2C server is active (I2C WebUI client detected and running).
bool zuluWebUIEnabled();

// Notify the WebUI that the SD card is ready (boot init or hot-swap reinit).
// If the I2C WebUI client is connected and subscribed, pushes the full filename cache now.
// If the I2C WebUI client is not yet subscribed, the cache is pushed when it subscribes.
void zuluWebUINotifySDCardReady();

// Notify the WebUI that the SD card has been removed.
// Pushes an empty filename list for each device so the I2C WebUI client clears its cache.
void zuluWebUINotifySDCardRemoved();

// Uploads a ZuluControl-firmware image (a raw .uf2 file) from the SD card to
// the WebUI/ZuluControl companion board over I2C, using a DMA-driven master
// transmit path with CRC32 verification and up to 3 retries per chunk. Call
// once at boot, after the I2C bus has been probed (g_i2c_claimed) and before
// zuluWebUIInit() -- independent of whether WiFi/WebUI itself is configured,
// since the companion board should be kept up to date regardless. Deletes
// `filename` from the SD card only once the client has confirmed (via a
// sentinel ACK) that it successfully flashed the update and is rebooting;
// otherwise leaves it in place so the upgrade is retried on the next boot.
void zuluWebUIUpgradeFirmware(const char* filename);
#endif