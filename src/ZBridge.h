/*
 * ZBridge integration for ZuluSCSI firmware
 * Copyright (c) 2026 Brendan Spear
 * GPL-2.0-or-later; ZuluSCSI is an independent third-party project.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

// Add the ZBridge vendor-specific USB bulk interface beside CDC. Call once
// after Serial.begin(), while the firmware is still completing startup.
void zbridge_usb_init();

// Poll the USB CDC binary protocol. Returns true while the serial stream is
// owned by ZBridge and the normal text console must not consume bytes.
bool zbridge_poll();

// Suppress normal target polling while an SD write or initiator session owns
// shared storage/SCSI resources.
bool zbridge_target_quiesced();

// Suppress firmware log output while framed binary traffic owns USB CDC.
bool zbridge_binary_active();

// Called from the accelerated initiator DATA OUT wait loop. It drains framed
// input for protocol v7, or services TinyUSB without consuming protocol-v8 raw
// PCM. It is safe while the Akai holds the SCSI bus in a DATA OUT phase.
void zbridge_stream_pump_usb();

// Protocol-v7/v8 Direct RAM keeps one Akai SEND command open while USB feeds
// the PIO/DMA provider. Ordinary command, SysEx and header writes keep the
// upstream pointer-backed path.
bool zbridge_streaming_write_active();

// Copy up to `capacity` bytes from either the framed credit ring or the exact
// checksummed raw USB window while the Akai holds DATA OUT. It returns zero
// only after a bounded stream failure/abort.
uint32_t zbridge_stream_read(uint8_t *destination, uint32_t capacity);

// Protocol-v9 Direct RAM keeps one Akai RECEIVE command open while the PIO
// reader forwards bounded DATA IN windows to native USB. The ordinary
// pointer-backed initiator read path remains untouched for headers and SysEx.
bool zbridge_streaming_read_active();

// Queue one exact SCSI DATA IN window on the vendor-specific USB endpoint.
// Returns false after a bounded disconnect, abort, or endpoint timeout.
bool zbridge_stream_write(const uint8_t *source, uint32_t count);
