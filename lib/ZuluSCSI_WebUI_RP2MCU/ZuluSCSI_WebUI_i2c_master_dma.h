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

// DMA-driven I2C master transmit path used specifically for sending
// ZuluControl-firmware upgrade chunks (I2C_SERVER_UPDATE_FW_DATA) to the
// WebUI/ZuluControl companion board. This is a direct port of ZuluIDE-firmware's
// lib/ZuluControl/include/zuluide/i2c/i2c_master_dma.h -- the companion board
// runs the same ZuluControl-firmware client for both ZuluIDE and ZuluSCSI (see
// its DeviceType::ZuluSCSI detection and the " ZuluSCSI" suffix sent in
// I2C_SERVER_API_VERSION), so the wire protocol here is intentionally
// byte-for-byte identical to ZuluIDE's.
//
// Everything else in the WebUI protocol (filenames/status/SSID/etc.) keeps
// using the existing blocking webuiI2CSend()/webuiI2CReceive() transceiver in
// ZuluSCSI_WebUI_I2CServer.h -- DMA is used only here, on the master transmit
// side, where it's proven to work (see i2c_master_dma.cpp in ZuluIDE-firmware
// for why slave-mode DMA on the client was abandoned).
//
// Wire protocol (big-endian throughout), unchanged from ZuluIDE:
//   Chunk-data message: [cmd][len_hi][len_lo][payload: up to WEBUI_FW_UPGRADE_MAX_CHUNK bytes]
//   Ack message:        [cmd][len_hi][len_lo][crc32: 4 bytes]
//
// CRC32 convention (must match the client's software CRC32 exactly): the same
// reflected CRC-32/ISO-HDLC algorithm used by this firmware's own crc32() in
// lib/SCSI2SD/src/firmware/network.c (not linked directly here -- that file is
// only compiled when ZULUSCSI_NETWORK is defined, which isn't true for every
// ZULUCONTROL_FIRMWARE board, so this is a self-contained copy of the same
// algorithm) -- table-driven, polynomial 0xEDB88320 (the bit-reversed form of
// 0x04C11DB7), seed WEBUI_FW_UPGRADE_CRC32_SEED, result XORed with
// WEBUI_FW_UPGRADE_CRC32_FINAL_XOR.

#pragma once
#ifdef ZULUCONTROL_FIRMWARE

#include <hardware/i2c.h>
#include <pico/time.h>
#include <cstdint>
#include <cstddef>

#define WEBUI_FW_UPGRADE_CRC32_SEED 0xFFFFFFFFu
#define WEBUI_FW_UPGRADE_CRC32_FINAL_XOR 0xFFFFFFFFu

// Maximum payload size for a single chunk-data message. Must not exceed the
// ZuluControl-firmware client's staging buffer (g_fwup_state.staged[2048] in
// its fw_upgrade.cpp) or its Packet::buffer[MAX_MSG_SIZE=2048].
#define WEBUI_FW_UPGRADE_MAX_CHUNK 2048

// Claims two free DMA channels (via dma_claim_unused_channel(), which is safe
// here because every DMA user elsewhere in this firmware -- SCSI accel, SDIO,
// audio -- properly registers its channels with dma_channel_claim() rather
// than reserving them by fixed-number convention only). Must be called once,
// with the same i2c_inst_t already used by the shared Wire instance (i2c1;
// see control.cpp), before webuiI2CDmaSendChunk() is used. Returns false if
// no DMA channels are free.
bool webuiI2CDmaInit(i2c_inst_t* i2c);

// Releases the DMA channels claimed by webuiI2CDmaInit(). The firmware-upgrade
// transport only runs once at boot, so channels are freed afterward rather
// than held for the rest of the program's life.
void webuiI2CDmaDeinit();

// Sends [cmd][len_hi][len_lo][payload...] to the WebUI client as a
// header-DMA-channel chained to a data-DMA-channel (the actual wire
// transfer), while separately computing the CRC32 of `payload` in software
// on the CPU while the DMA transfer is in flight. Blocks until the transfer
// completes or `until` is reached. Returns false on any I2C-level abort
// (NACK/timeout); `*out_sent_crc` is only valid when this returns true. The
// client independently computes its own CRC32 of what it actually received
// and reports it in the ACK -- the caller is responsible for comparing that
// against `*out_sent_crc` to decide whether to retry.
bool webuiI2CDmaSendChunk(uint8_t cmd, const uint8_t* payload, uint16_t length,
                          uint32_t* out_sent_crc, absolute_time_t until);

#endif
