/*
 * The self-flash bootloader never exposes ZBridge protocol services. Keep the
 * four integration symbols available to shared ZuluSCSI objects without
 * linking the ~100 KiB application parser/queues into the 128 KiB bootloader.
 */

#include "ZBridge.h"
#include "ZBridgeFastHostWrite.h"
#include "ZBridgeFastHostRead.h"

bool zbridge_poll() { return false; }
bool zbridge_target_quiesced() { return false; }
bool zbridge_binary_active() { return false; }
void zbridge_stream_pump_usb() {}
bool zbridge_streaming_write_active() { return false; }
uint32_t zbridge_stream_read(uint8_t *, uint32_t) { return 0; }
bool zbridge_streaming_read_active() { return false; }
bool zbridge_stream_write(const uint8_t *, uint32_t) { return false; }
extern "C" bool zbridge_native_in_stream_active() { return false; }

uint32_t zbridgeFastHostWrite(const uint8_t *, uint32_t,
                              volatile int *) {
    return UINT32_MAX;
}
uint32_t zbridgeFastHostStreamWrite(uint32_t, volatile int *) {
    return UINT32_MAX;
}
uint32_t zbridgeFastHostStreamRead(uint32_t, int *, int, volatile int *) {
    return UINT32_MAX;
}
