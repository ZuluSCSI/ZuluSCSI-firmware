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

// I2C protocol constants and low-level transceiver for ZuluSCSI WebUI.
// ZuluSCSI main MCU is I2C master; WebUI board running ZuluSCSI-Remote-Interface
// is I2C slave at address 0x45.
//
// Packet format (both directions):
//   [command:1] [length_hi:1] [length_lo:1] [payload:length]
// Empty payload: length = 0x0000.

#pragma once
#ifdef ZULUCONTROL_FIRMWARE

#include <stdint.h>
#include <stddef.h>

// I2C slave address of the WebUI board
#define WEBUI_I2C_SLAVE_ADDR     0x45

// Maximum payload length accepted on the receive path
#define WEBUI_MAX_MSG_SIZE       512

// Protocol version string - must match ZuluSCSI_WebUI_I2CClient.h
#define WEBUI_I2C_API_VERSION    "4.0.0"

// ── Server → client commands (ZuluSCSI sends to WebUI board) ────────────────
#define I2C_SERVER_API_VERSION            0x01  // API version string
#define I2C_SERVER_WIFI_CONNECT           0x02  // Tell I2C WebUI client to connect to WiFi
#define I2C_SERVER_UPDATE_FW_REQUEST      0x03  // Start/finish/abort/retry a ZuluControl-firmware upgrade; payload[0]=WebUIFwUpgradeRequest
#define I2C_SERVER_UPDATE_FW_DATA         0x04  // A chunk of ZuluControl-firmware data (DMA transport, see ZuluSCSI_WebUI_i2c_master_dma.h)
#define I2C_SERVER_UPDATE_FILENAME_CACHE  0x08  // Announce start of filename list
#define I2C_SERVER_IMAGE_FILENAME         0x09  // One filename; payload[0]=scsi_id, [1..]=name; empty=done
#define I2C_SERVER_SYSTEM_STATUS_JSON     0x0A  // System status as JSON object
#define I2C_SERVER_IMAGE_JSON             0x0B  // One image as JSON object; payload[0]=scsi_id, [1..]=json
#define I2C_SERVER_SSID                   0x0D  // WiFi SSID string
#define I2C_SERVER_SSID_PASS              0x0E  // WiFi password string
#define I2C_SERVER_RESET                  0x0F  // Reset client state
#define I2C_SERVER_STATIC_IP              0x10  // Static IP prefixed with "ip"/"nm"/"gw"
#define I2C_SERVER_IP_ADDRESS_ACK         0x11  // Acknowledge I2C WebUI client IP address
#define I2C_SERVER_DEVICE_LIST_JSON       0x12  // JSON array of removable device descriptors
#define I2C_SERVER_SD_STATUS_CHANGE       0x13  // SD card presence changed; payload[0] = 0x00 not present, 0x01 present

#define I2C_SERVER_SD_NOT_PRESENT 0x00
#define I2C_SERVER_SD_PRESENT     0x01

// ── Client → server commands (WebUI board sends to ZuluSCSI) ────────────────
#define I2C_CLIENT_NOOP                   0x00  // No operation (slave queue empty)
#define I2C_CLIENT_API_VERSION            0x01  // I2C WebUI client API version string
#define I2C_CLIENT_UPDATE_FW_ACK          0x03  // Ack for a firmware chunk; payload=[len_hi][len_lo][crc32:4]. Sentinel len=0xFFFF/crc=0 after FINISH means the client flashed successfully and is rebooting.
#define I2C_CLIENT_FETCH_FILENAMES        0x09  // Request filename list; payload[0]=scsi_id
#define I2C_CLIENT_SUBSCRIBE_STATUS_JSON  0x0A  // Subscribe to periodic status updates
#define I2C_CLIENT_LOAD_IMAGE             0x0B  // Mount image; payload[0]=scsi_id, [1..]=full_path
#define I2C_CLIENT_EJECT_IMAGE            0x0C  // Eject media; payload[0]=scsi_id
#define I2C_CLIENT_FETCH_IMAGES_JSON      0x0D  // Request all image JSON; payload[0]=scsi_id
#define I2C_CLIENT_FETCH_SSID             0x0E  // Request WiFi SSID
#define I2C_CLIENT_FETCH_SSID_PASS        0x0F  // Request WiFi password
#define I2C_CLIENT_FETCH_ITR_IMAGE        0x10  // Request next image (iterator); payload[0]=scsi_id
#define I2C_CLIENT_IP_ADDRESS             0x11  // I2C WebUI client reports its IP address
#define I2C_CLIENT_LOG_MSG                0x12  // Log message from I2C WebUI client ('n'/'d' prefix)
#define I2C_CLIENT_FETCH_DEVICE_LIST      0x13  // Request list of removable SCSI devices
#define I2C_CLIENT_INSERT_MEDIA           0x14  // Close tray / insert media; payload[0]=scsi_id

// ── Low-level I2C transceiver ────────────────────────────────────────────────

// Send a command with optional payload to the WebUI slave.
// Returns true on successful ACK.
bool webuiI2CSend(uint8_t command, const uint8_t *payload = nullptr, uint16_t length = 0);

// Convenience overload for string payloads.
bool webuiI2CSend(uint8_t command, const char *str);

// Poll the WebUI slave for a queued message.
// Returns true if a message was received; cmd_out/payload_out/length_out are filled.
// payload_out must point to at least WEBUI_MAX_MSG_SIZE+1 bytes.
// Returns false if the slave is absent (NAK) or sent NOOP.
bool webuiI2CReceive(uint8_t *cmd_out, uint8_t *payload_out, uint16_t *length_out);

// Returns true if the WebUI board answered the last receive attempt without error.
bool webuiI2CWebUIPresent();

// Probe for the WebUI I2C client; sets g_i2c_claimed if the slave ACKs.
void webuiI2CDetectClient();
#endif