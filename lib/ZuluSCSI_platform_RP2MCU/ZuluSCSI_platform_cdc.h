/**
 * ZuluSCSI USB CDC Platform Layer
 *
 * Abstracts USB CDC (virtual serial port) functionality for the
 * USB bridge. Uses TinyUSB under the hood.
 *
 * Copyright (c) 2025
 * Licensed under GPL-3.0
 */

#ifndef ZULUSCSI_PLATFORM_CDC_H
#define ZULUSCSI_PLATFORM_CDC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// USB Device Identification
// =============================================================================

#define USB_BRIDGE_VID          0x1209      // pid.codes
#define USB_BRIDGE_PID          0x2053      // ZuluSCSI Bridge (to be registered)
#define USB_BRIDGE_DEVICE_NAME  "ZuluSCSI Bridge"

// =============================================================================
// Buffer Sizes
// =============================================================================

#define CDC_RX_BUFFER_SIZE      4096
#define CDC_TX_BUFFER_SIZE      4096

// =============================================================================
// API Functions
// =============================================================================

/**
 * Initialize the USB CDC subsystem.
 * Should be called after platform_init() and before usb_bridge_init().
 *
 * @return true if USB initialized successfully
 */
bool platform_cdc_init(void);

/**
 * Check if USB CDC is connected and DTR is set.
 * DTR indicates the host has opened the port.
 */
bool platform_cdc_connected(void);

/**
 * Get number of bytes available to read.
 */
uint32_t platform_cdc_available(void);

/**
 * Read data from USB CDC.
 *
 * @param buffer    Buffer to read into
 * @param length    Maximum bytes to read
 * @return          Number of bytes actually read
 */
uint32_t platform_cdc_read(uint8_t* buffer, uint32_t length);

/**
 * Read a single byte from USB CDC.
 *
 * @param byte      Output byte
 * @return          true if byte was read, false if buffer empty
 */
bool platform_cdc_read_byte(uint8_t* byte);

/**
 * Write data to USB CDC.
 *
 * @param buffer    Data to write
 * @param length    Number of bytes to write
 * @return          Number of bytes actually written
 */
uint32_t platform_cdc_write(const uint8_t* buffer, uint32_t length);

/**
 * Write data and flush immediately.
 * Use for small packets that should be sent right away.
 *
 * @param buffer    Data to write
 * @param length    Number of bytes to write
 * @return          Number of bytes actually written
 */
uint32_t platform_cdc_write_flush(const uint8_t* buffer, uint32_t length);

/**
 * Flush pending write data.
 * Ensures all buffered data is sent over USB.
 */
void platform_cdc_flush(void);

/**
 * Discard all data in receive buffer.
 */
void platform_cdc_rx_flush(void);

/**
 * Get available space in transmit buffer.
 */
uint32_t platform_cdc_write_available(void);

/**
 * Check if transmit buffer is empty (all data sent).
 */
bool platform_cdc_tx_empty(void);

/**
 * Poll USB task.
 * Must be called periodically to process USB events.
 * This is typically done in platform_poll().
 */
void platform_cdc_task(void);

/**
 * Set RTS callback (for flow control notification).
 * Called when host changes RTS state.
 *
 * @param callback  Function to call, or NULL to disable
 */
typedef void (*platform_cdc_rts_callback_t)(bool rts_state);
void platform_cdc_set_rts_callback(platform_cdc_rts_callback_t callback);

/**
 * Set connection callback.
 * Called when host connects or disconnects.
 *
 * @param callback  Function to call, or NULL to disable
 */
typedef void (*platform_cdc_connect_callback_t)(bool connected);
void platform_cdc_set_connect_callback(platform_cdc_connect_callback_t callback);

/**
 * Get USB VID/PID as currently configured.
 *
 * @param vid   Output for Vendor ID
 * @param pid   Output for Product ID
 */
void platform_cdc_get_vid_pid(uint16_t* vid, uint16_t* pid);

#ifdef __cplusplus
}
#endif

#endif // ZULUSCSI_PLATFORM_CDC_H
