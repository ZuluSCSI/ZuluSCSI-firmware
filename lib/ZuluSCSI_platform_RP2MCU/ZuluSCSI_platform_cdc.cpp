/**
 * ZuluSCSI USB CDC Platform Layer - Implementation
 *
 * Abstracts USB CDC functionality using TinyUSB.
 *
 * Copyright (c) 2025
 * Licensed under GPL-3.0
 */

#include "ZuluSCSI_platform_cdc.h"
#include "ZuluSCSI_log.h"

// TinyUSB headers
#include "tusb.h"

// =============================================================================
// Internal State
// =============================================================================

static struct {
    bool initialized;
    bool connected;
    bool dtr_state;
    bool rts_state;
    platform_cdc_rts_callback_t rts_callback;
    platform_cdc_connect_callback_t connect_callback;
} g_cdc;

// =============================================================================
// TinyUSB Callbacks
// =============================================================================

// Invoked when CDC line state changes (DTR, RTS)
extern "C" void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    (void)itf;

    bool was_connected = g_cdc.connected;
    g_cdc.dtr_state = dtr;
    g_cdc.rts_state = rts;

    // Consider connected when DTR is set (host opened port)
    g_cdc.connected = dtr;

    // Notify on RTS change
    if (g_cdc.rts_callback && rts != g_cdc.rts_state) {
        g_cdc.rts_callback(rts);
    }

    // Notify on connection change
    if (g_cdc.connect_callback && g_cdc.connected != was_connected) {
        g_cdc.connect_callback(g_cdc.connected);
        if (g_cdc.connected) {
            logmsg("USB CDC connected");
        } else {
            logmsg("USB CDC disconnected");
        }
    }
}

// Invoked when CDC receives data
extern "C" void tud_cdc_rx_cb(uint8_t itf)
{
    (void)itf;
    // Data is buffered by TinyUSB, we'll read it in platform_cdc_read()
}

// Invoked when CDC transmission complete
extern "C" void tud_cdc_tx_complete_cb(uint8_t itf)
{
    (void)itf;
    // Can use for flow control if needed
}

// =============================================================================
// API Implementation
// =============================================================================

bool platform_cdc_init(void)
{
    memset(&g_cdc, 0, sizeof(g_cdc));

    // TinyUSB initialization is handled by platform_init()
    // We just set up our state
    g_cdc.initialized = true;

    logmsg("USB CDC initialized");
    return true;
}

bool platform_cdc_connected(void)
{
    return g_cdc.initialized && g_cdc.connected && tud_cdc_connected();
}

uint32_t platform_cdc_available(void)
{
    if (!g_cdc.initialized) return 0;
    return tud_cdc_available();
}

uint32_t platform_cdc_read(uint8_t* buffer, uint32_t length)
{
    if (!g_cdc.initialized || !buffer || length == 0) {
        return 0;
    }

    // TinyUSB provides blocking and non-blocking read
    // Use non-blocking for our polling model
    uint32_t count = tud_cdc_read(buffer, length);
    return count;
}

bool platform_cdc_read_byte(uint8_t* byte)
{
    if (!g_cdc.initialized || !byte) {
        return false;
    }

    if (tud_cdc_available() > 0) {
        *byte = tud_cdc_read_char();
        return true;
    }
    return false;
}

uint32_t platform_cdc_write(const uint8_t* buffer, uint32_t length)
{
    if (!g_cdc.initialized || !buffer || length == 0) {
        return 0;
    }

    if (!platform_cdc_connected()) {
        return 0;
    }

    // Write to TinyUSB FIFO
    uint32_t written = 0;
    while (written < length) {
        uint32_t available = tud_cdc_write_available();
        if (available == 0) {
            // Buffer full, flush and retry
            tud_cdc_write_flush();
            tud_task();  // Process USB events

            available = tud_cdc_write_available();
            if (available == 0) {
                // Still no space, return what we've written
                break;
            }
        }

        uint32_t to_write = length - written;
        if (to_write > available) {
            to_write = available;
        }

        uint32_t n = tud_cdc_write(buffer + written, to_write);
        written += n;

        if (n == 0) {
            break;  // Something went wrong
        }
    }

    return written;
}

uint32_t platform_cdc_write_flush(const uint8_t* buffer, uint32_t length)
{
    uint32_t written = platform_cdc_write(buffer, length);
    platform_cdc_flush();
    return written;
}

void platform_cdc_flush(void)
{
    if (!g_cdc.initialized) return;
    tud_cdc_write_flush();
}

void platform_cdc_rx_flush(void)
{
    if (!g_cdc.initialized) return;

    // Read and discard all pending data
    while (tud_cdc_available() > 0) {
        tud_cdc_read_char();
    }
}

uint32_t platform_cdc_write_available(void)
{
    if (!g_cdc.initialized) return 0;
    return tud_cdc_write_available();
}

bool platform_cdc_tx_empty(void)
{
    if (!g_cdc.initialized) return true;
    return tud_cdc_write_available() == CFG_TUD_CDC_TX_BUFSIZE;
}

void platform_cdc_task(void)
{
    if (!g_cdc.initialized) return;
    tud_task();
}

void platform_cdc_set_rts_callback(platform_cdc_rts_callback_t callback)
{
    g_cdc.rts_callback = callback;
}

void platform_cdc_set_connect_callback(platform_cdc_connect_callback_t callback)
{
    g_cdc.connect_callback = callback;
}

void platform_cdc_get_vid_pid(uint16_t* vid, uint16_t* pid)
{
    if (vid) *vid = USB_BRIDGE_VID;
    if (pid) *pid = USB_BRIDGE_PID;
}
