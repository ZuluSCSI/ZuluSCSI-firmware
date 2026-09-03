#pragma once

#include <stdint.h>

// RP2350/Blaster-only accelerated initiator DATA OUT. The function preserves
// the ordinary SCSI async REQ/ACK contract but lets PIO own the handshake and
// DMA feed parity-expanded words. UINT32_MAX means the accelerated path is not
// available and the caller should use the upstream byte loop.
uint32_t zbridgeFastHostWrite(const uint8_t *data, uint32_t count,
                              volatile int *resetFlag);

// Continuous variant used while a protocol-v7 bulk stream owns DATA OUT.
// Bytes are pulled from zbridge_stream_read() instead of a single resident
// host buffer, allowing one Akai SEND command to span many USB frames.
uint32_t zbridgeFastHostStreamWrite(uint32_t count,
                                    volatile int *resetFlag);
