/**
 * ZuluSCSI USB Bridge Protocol Handler - Implementation
 *
 * Handles USB communication protocol for Mac-to-Akai transfers.
 *
 * Copyright (c) 2025
 * Licensed under GPL-3.0
 */

#include "ZuluSCSI_usb_bridge.h"
#include "ZuluSCSI_ram_overlay.h"
#include "ZuluSCSI_platform_cdc.h"
#include "ZuluSCSI_log.h"
#include "ZuluSCSI_config.h"

#include <string.h>

// Forward declarations for ZuluSCSI integration
extern "C" {
    // From ZuluSCSI_disk.cpp
    bool scsiDiskReadBlocks(uint32_t lba, uint8_t* buffer, uint32_t count);
    bool scsiDiskWriteBlocks(uint32_t lba, const uint8_t* buffer, uint32_t count);
    uint32_t scsiDiskGetCapacity(void);
    bool scsiDiskIsPresent(void);

    // From scsiPhy or platform
    extern uint8_t scsi_id;
    extern volatile uint8_t scsiDev_phase;
}

// Optional MIDI adapter seam. Stock firmware remains honest: without a board
// integration overriding these weak hooks, no MIDI capability is advertised
// and MIDI commands return ERR_MIDI_UNAVAILABLE.
extern "C" bool __attribute__((weak)) zbridge_midi_available(void) { return false; }
extern "C" bool __attribute__((weak)) zbridge_midi_input_available(void) { return false; }
extern "C" bool __attribute__((weak)) zbridge_midi_write(uint8_t, uint32_t, const uint8_t*, uint16_t) { return false; }
extern "C" void __attribute__((weak)) zbridge_midi_flush(uint8_t) {}
extern "C" bool __attribute__((weak)) zbridge_akai_discovery_available(void) { return false; }
extern "C" bool __attribute__((weak)) zbridge_akai_discover(uint8_t, uint8_t,
                                                              usb_bridge_akai_device_t*) { return false; }

// =============================================================================
// Configuration
// =============================================================================

#define RX_BUFFER_SIZE      USB_BRIDGE_MAX_PACKET
#define TX_BUFFER_SIZE      USB_BRIDGE_MAX_PACKET
#define BLOCK_BUFFER_SIZE   (USB_BRIDGE_MAX_BLOCKS_PER_CMD * USB_BRIDGE_BLOCK_SIZE)

// Firmware version
#define FW_VERSION_MAJOR    1
#define FW_VERSION_MINOR    0
#define FW_VERSION_PATCH    0
#define FW_VERSION_BUILD    1

// =============================================================================
// Internal State
// =============================================================================

typedef enum {
    RX_STATE_SYNC1,
    RX_STATE_SYNC2,
    RX_STATE_LENGTH_LOW,
    RX_STATE_LENGTH_HIGH,
    RX_STATE_PAYLOAD,
} rx_state_t;

static struct {
    // Receive state machine
    rx_state_t  rx_state;
    uint16_t    rx_length;
    uint16_t    rx_received;
    uint8_t     rx_buffer[RX_BUFFER_SIZE];

    // Transmit buffer
    uint8_t     tx_buffer[TX_BUFFER_SIZE];

    // Block buffer for read/write operations
    uint8_t     block_buffer[BLOCK_BUFFER_SIZE];

    // Event subscription
    uint16_t    event_mask;

    // State flags
    bool        initialized;
    bool        debug_enabled;
    bool        connected;

    // SCSI state tracking (for notifications)
    uint8_t     last_scsi_command;
    uint32_t    last_scsi_lba;
    uint16_t    last_scsi_count;

} g_bridge;

// =============================================================================
// CRC-16-CCITT
// =============================================================================

static uint16_t crc16_ccitt(const uint8_t* data, size_t length)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

// =============================================================================
// Packet Transmission
// =============================================================================

/**
 * Send a complete packet.
 */
static bool send_packet(uint8_t opcode, const uint8_t* payload, uint16_t payload_len)
{
    if (!platform_cdc_connected()) {
        return false;
    }

    // Build packet header
    uint16_t length = 1 + payload_len;  // opcode + payload
    g_bridge.tx_buffer[0] = 0xAA;
    g_bridge.tx_buffer[1] = 0x55;
    g_bridge.tx_buffer[2] = length & 0xFF;
    g_bridge.tx_buffer[3] = (length >> 8) & 0xFF;
    g_bridge.tx_buffer[4] = opcode;

    // Copy payload
    if (payload && payload_len > 0) {
        memcpy(g_bridge.tx_buffer + 5, payload, payload_len);
    }

    // ZBridge v3 CRC covers opcode + payload only.
    uint16_t crc = crc16_ccitt(g_bridge.tx_buffer + 4, length);
    g_bridge.tx_buffer[5 + payload_len] = crc & 0xFF;
    g_bridge.tx_buffer[6 + payload_len] = (crc >> 8) & 0xFF;

    // Total packet size: sync(2) + length(2) + opcode(1) + payload + crc(2)
    uint32_t total = 7 + payload_len;

    uint32_t written = platform_cdc_write_flush(g_bridge.tx_buffer, total);
    return written == total;
}

/**
 * Send an error response.
 */
static void send_error(usb_bridge_error_t code, uint8_t context, const char* message)
{
    usb_bridge_error_response_t err;
    memset(&err, 0, sizeof(err));
    err.error_code = code;
    err.context = context;
    if (message) {
        strncpy(err.message, message, sizeof(err.message) - 1);
    }
    send_packet(RSP_ERROR, (uint8_t*)&err, sizeof(err));
}

// =============================================================================
// Command Handlers
// =============================================================================

static void handle_ping(void)
{
    send_packet(RSP_PONG, nullptr, 0);
}

static void handle_get_info(void)
{
    usb_bridge_device_info_t info;
    memset(&info, 0, sizeof(info));

    info.firmware_ver = ((uint32_t)FW_VERSION_BUILD << 24) |
                        ((uint32_t)FW_VERSION_PATCH << 16) |
                        ((uint32_t)FW_VERSION_MINOR << 8) |
                        FW_VERSION_MAJOR;
    info.protocol_ver = USB_BRIDGE_PROTOCOL_VERSION;
    info.capabilities = FLAG_OVERLAY_SUPPORTED | FLAG_EVENTS_SUPPORTED;
    if (zbridge_midi_available()) info.capabilities |= FLAG_MIDI_STREAM;
    if (zbridge_midi_input_available()) info.capabilities |= FLAG_MIDI_INPUT;
    if (zbridge_akai_discovery_available()) info.capabilities |= FLAG_AKAI_SCSI_DISCOVERY;
    info.scsi_id = scsi_id;
    info.num_images = 1;  // TODO: Support multiple images
    info.active_image = 0;
    strncpy(info.label, "ZBridge", sizeof(info.label) - 1);

    send_packet(RSP_DEVICE_INFO, (uint8_t*)&info, sizeof(info));
}

static void handle_read_blocks(const uint8_t* payload, uint16_t len)
{
    if (len < sizeof(usb_bridge_read_request_t)) {
        send_error(ERR_INVALID_LENGTH, CMD_READ_BLOCKS, "Request too short");
        return;
    }

    const usb_bridge_read_request_t* req = (const usb_bridge_read_request_t*)payload;
    uint32_t lba = req->start_lba;
    uint16_t count = req->block_count;

    // Validate
    if (count == 0 || count > USB_BRIDGE_MAX_BLOCKS_PER_CMD) {
        send_error(ERR_BLOCK_COUNT, CMD_READ_BLOCKS, "Invalid block count");
        return;
    }

    uint32_t capacity = scsiDiskGetCapacity();
    if (lba + count > capacity) {
        send_error(ERR_INVALID_LBA, CMD_READ_BLOCKS, "LBA out of range");
        return;
    }

    // Read blocks - check overlay first, then SD
    uint8_t* buffer = g_bridge.block_buffer;
    for (uint16_t i = 0; i < count; i++) {
        uint32_t block_lba = lba + i;
        uint8_t* block_ptr = buffer + (i * USB_BRIDGE_BLOCK_SIZE);

        if (!ram_overlay_read(block_lba, block_ptr)) {
            // Not in overlay, read from SD
            if (!scsiDiskReadBlocks(block_lba, block_ptr, 1)) {
                send_error(ERR_SD_READ, CMD_READ_BLOCKS, "SD read failed");
                return;
            }
        }
    }

    // Build response
    // Header: start_lba (4) + block_count (2) = 6 bytes
    // Data: count * 512 bytes
    uint16_t data_size = count * USB_BRIDGE_BLOCK_SIZE;
    uint16_t total_payload = 6 + data_size;

    // We need to send this in chunks since it may exceed TX buffer
    // For now, we'll build a temporary buffer
    // Note: This limits us to 8 blocks max to fit in buffer

    uint8_t* response = g_bridge.tx_buffer + 5;  // After header
    response[0] = lba & 0xFF;
    response[1] = (lba >> 8) & 0xFF;
    response[2] = (lba >> 16) & 0xFF;
    response[3] = (lba >> 24) & 0xFF;
    response[4] = count & 0xFF;
    response[5] = (count >> 8) & 0xFF;
    memcpy(response + 6, buffer, data_size);

    send_packet(RSP_BLOCK_DATA, response, total_payload);
}

static void handle_write_blocks(const uint8_t* payload, uint16_t len)
{
    if (len < sizeof(usb_bridge_write_request_t)) {
        send_error(ERR_INVALID_LENGTH, CMD_WRITE_BLOCKS, "Request too short");
        return;
    }

    const usb_bridge_write_request_t* req = (const usb_bridge_write_request_t*)payload;
    uint32_t lba = req->start_lba;
    uint16_t count = req->block_count;

    // Validate
    if (count == 0 || count > USB_BRIDGE_MAX_BLOCKS_PER_CMD) {
        send_error(ERR_BLOCK_COUNT, CMD_WRITE_BLOCKS, "Invalid block count");
        return;
    }

    uint16_t expected_len = 6 + (count * USB_BRIDGE_BLOCK_SIZE);
    if (len < expected_len) {
        send_error(ERR_INVALID_LENGTH, CMD_WRITE_BLOCKS, "Data too short");
        return;
    }

    uint32_t capacity = scsiDiskGetCapacity();
    if (lba + count > capacity) {
        send_error(ERR_INVALID_LBA, CMD_WRITE_BLOCKS, "LBA out of range");
        return;
    }

    // Check if SCSI bus is busy
    if (scsiDev_phase != SCSI_PHASE_BUS_FREE) {
        send_error(ERR_SCSI_BUSY, CMD_WRITE_BLOCKS, "SCSI transfer in progress");
        return;
    }

    // Write blocks to overlay
    const uint8_t* data = payload + 6;
    for (uint16_t i = 0; i < count; i++) {
        uint32_t block_lba = lba + i;
        const uint8_t* block_data = data + (i * USB_BRIDGE_BLOCK_SIZE);

        if (!ram_overlay_write(block_lba, block_data, OVERLAY_SOURCE_MAC)) {
            send_error(ERR_OVERLAY_FULL, CMD_WRITE_BLOCKS, "RAM overlay full");
            return;
        }
    }

    // Send acknowledgment
    usb_bridge_write_ack_t ack;
    ack.start_lba = lba;
    ack.block_count = count;
    ack.overlay_used = ram_overlay_get_used();

    send_packet(RSP_WRITE_ACK, (uint8_t*)&ack, sizeof(ack));
}

static void handle_sync_to_sd(const uint8_t* payload, uint16_t len)
{
    uint8_t flags = 0;
    if (len >= 1) {
        flags = payload[0];
    }

    bool clear_after = (flags & SYNC_FLAG_CLEAR_AFTER) != 0;
    bool verify = (flags & SYNC_FLAG_VERIFY) != 0;

    uint16_t blocks_synced = 0;
    uint16_t errors = 0;

    uint8_t status = ram_overlay_sync_to_sd(clear_after, verify, &blocks_synced, &errors);

    usb_bridge_sync_result_t result;
    result.status = status;
    result.blocks_written = blocks_synced;
    result.errors = errors;

    send_packet(RSP_SYNC_COMPLETE, (uint8_t*)&result, sizeof(result));
}

static void handle_get_dirty_blocks(const uint8_t* payload, uint16_t len)
{
    uint8_t filter = 0;
    uint16_t max_count = 256;

    if (len >= sizeof(usb_bridge_dirty_request_t)) {
        const usb_bridge_dirty_request_t* req = (const usb_bridge_dirty_request_t*)payload;
        filter = req->filter;
        max_count = req->max_count;
    }

    // Limit to reasonable maximum
    if (max_count > 256) max_count = 256;

    // Get total count
    uint16_t total_count = ram_overlay_get_dirty_count(filter);

    // Get LBA list
    uint32_t lba_list[256];
    uint16_t returned_count = ram_overlay_get_dirty_list(filter, lba_list, max_count);

    // Build response
    uint16_t payload_size = 4 + (returned_count * 4);
    uint8_t* response = g_bridge.tx_buffer + 5;

    response[0] = total_count & 0xFF;
    response[1] = (total_count >> 8) & 0xFF;
    response[2] = returned_count & 0xFF;
    response[3] = (returned_count >> 8) & 0xFF;

    memcpy(response + 4, lba_list, returned_count * 4);

    send_packet(RSP_DIRTY_BLOCK_LIST, response, payload_size);
}

static void handle_clear_overlay(void)
{
    ram_overlay_clear();
    send_packet(RSP_PONG, nullptr, 0);
}

static void handle_get_scsi_status(void)
{
    usb_bridge_scsi_status_t status;
    memset(&status, 0, sizeof(status));

    status.phase = scsiDev_phase;
    status.last_command = g_bridge.last_scsi_command;
    status.last_lba = g_bridge.last_scsi_lba;
    status.last_count = g_bridge.last_scsi_count;
    status.transfer_active = (scsiDev_phase != SCSI_PHASE_BUS_FREE) ? 1 : 0;

    send_packet(RSP_SCSI_STATUS, (uint8_t*)&status, sizeof(status));
}

static void handle_subscribe_events(const uint8_t* payload, uint16_t len)
{
    if (len >= sizeof(usb_bridge_subscribe_request_t)) {
        const usb_bridge_subscribe_request_t* req = (const usb_bridge_subscribe_request_t*)payload;
        g_bridge.event_mask = req->event_mask;
        dbgmsg("Event mask set to ", g_bridge.event_mask);
    }

    send_packet(RSP_PONG, nullptr, 0);
}

static void handle_set_active_image(const uint8_t* payload, uint16_t len)
{
    if (len < 1) {
        send_error(ERR_INVALID_LENGTH, CMD_SET_ACTIVE_IMAGE, "Missing index");
        return;
    }

    uint8_t index = payload[0];

    // TODO: Implement multi-image support
    if (index != 0) {
        send_error(ERR_IMAGE_INDEX, CMD_SET_ACTIVE_IMAGE, "Invalid image index");
        return;
    }

    send_packet(RSP_PONG, nullptr, 0);
}

static void handle_get_overlay_stats(void)
{
    overlay_stats_t stats;
    ram_overlay_get_stats(&stats);

    usb_bridge_overlay_stats_t response;
    response.capacity = stats.capacity;
    response.used = stats.used;
    response.mac_blocks = stats.mac_blocks;
    response.akai_blocks = stats.akai_blocks;
    response.dirty_blocks = stats.dirty_blocks;
    response.total_reads = stats.total_reads;
    response.total_writes = stats.total_writes;
    response.cache_hits = stats.cache_hits;

    send_packet(RSP_OVERLAY_STATS, (uint8_t*)&response, sizeof(response));
}

static void handle_read_overlay_only(const uint8_t* payload, uint16_t len)
{
    if (len < sizeof(usb_bridge_read_request_t)) {
        send_error(ERR_INVALID_LENGTH, CMD_READ_OVERLAY_ONLY, "Request too short");
        return;
    }

    const usb_bridge_read_request_t* req = (const usb_bridge_read_request_t*)payload;
    uint32_t lba = req->start_lba;
    uint16_t count = req->block_count;

    // For overlay-only read, we return only blocks that are in the overlay
    // Others are skipped (not read from SD)

    uint8_t* buffer = g_bridge.block_buffer;
    uint16_t found_count = 0;

    for (uint16_t i = 0; i < count && found_count < count; i++) {
        uint32_t block_lba = lba + i;

        if (ram_overlay_read(block_lba, buffer + (found_count * USB_BRIDGE_BLOCK_SIZE))) {
            found_count++;
        }
    }

    // Build response with actual blocks found
    uint16_t data_size = found_count * USB_BRIDGE_BLOCK_SIZE;
    uint16_t total_payload = 6 + data_size;

    uint8_t* response = g_bridge.tx_buffer + 5;
    response[0] = lba & 0xFF;
    response[1] = (lba >> 8) & 0xFF;
    response[2] = (lba >> 16) & 0xFF;
    response[3] = (lba >> 24) & 0xFF;
    response[4] = found_count & 0xFF;
    response[5] = (found_count >> 8) & 0xFF;
    memcpy(response + 6, buffer, data_size);

    send_packet(RSP_BLOCK_DATA, response, total_payload);
}

static void handle_invalidate_block(const uint8_t* payload, uint16_t len)
{
    if (len < sizeof(usb_bridge_invalidate_request_t)) {
        send_error(ERR_INVALID_LENGTH, CMD_INVALIDATE_BLOCK, "Request too short");
        return;
    }

    const usb_bridge_invalidate_request_t* req = (const usb_bridge_invalidate_request_t*)payload;
    ram_overlay_invalidate(req->lba);

    send_packet(RSP_PONG, nullptr, 0);
}

static void send_v3_ack(const uint8_t* extra = nullptr, uint16_t extra_len = 0)
{
    uint8_t* response = g_bridge.tx_buffer + 5;
    response[0] = ERR_NONE;
    if (extra && extra_len > 0) memcpy(response + 1, extra, extra_len);
    send_packet(RSP_WRITE_ACK, response, 1 + extra_len);
}

static uint16_t read_u16_le(const uint8_t* data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_u32_le(const uint8_t* data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void handle_midi_send_batch(const uint8_t* payload, uint16_t len)
{
    if (!zbridge_midi_available()) {
        send_error(ERR_MIDI_UNAVAILABLE, CMD_MIDI_SEND_BATCH, "No MIDI adapter");
        return;
    }
    if (len < 4) {
        send_error(ERR_INVALID_MIDI, CMD_MIDI_SEND_BATCH, "Batch too short");
        return;
    }
    const uint8_t port = payload[0];
    const uint16_t count = read_u16_le(payload + 2);
    uint16_t offset = 4;
    uint16_t accepted = 0;
    for (uint16_t i = 0; i < count; ++i) {
        if ((uint32_t)offset + 6 > len) {
            send_error(ERR_INVALID_MIDI, CMD_MIDI_SEND_BATCH, "Truncated event");
            return;
        }
        const uint32_t timestamp_us = read_u32_le(payload + offset);
        const uint16_t event_len = read_u16_le(payload + offset + 4);
        offset += 6;
        if (event_len == 0 || (uint32_t)offset + event_len > len) {
            send_error(ERR_INVALID_MIDI, CMD_MIDI_SEND_BATCH, "Invalid event size");
            return;
        }
        if (!zbridge_midi_write(port, timestamp_us, payload + offset, event_len)) {
            send_error(ERR_MIDI_QUEUE_FULL, CMD_MIDI_SEND_BATCH, "MIDI adapter rejected event");
            return;
        }
        offset += event_len;
        ++accepted;
    }
    if (offset != len) {
        send_error(ERR_INVALID_MIDI, CMD_MIDI_SEND_BATCH, "Trailing MIDI bytes");
        return;
    }
    uint8_t receipt[4] = {
        (uint8_t)(accepted & 0xFF), (uint8_t)(accepted >> 8),
        0, 0 // synchronous adapter queue depth
    };
    send_v3_ack(receipt, sizeof(receipt));
}

static void handle_midi_flush(const uint8_t* payload, uint16_t len)
{
    if (!zbridge_midi_available()) {
        send_error(ERR_MIDI_UNAVAILABLE, CMD_MIDI_FLUSH, "No MIDI adapter");
        return;
    }
    if (len != 1) {
        send_error(ERR_INVALID_LENGTH, CMD_MIDI_FLUSH, "Missing MIDI port");
        return;
    }
    zbridge_midi_flush(payload[0]);
    send_v3_ack();
}

static void handle_akai_discover(const uint8_t* payload, uint16_t len)
{
    if (!zbridge_akai_discovery_available()) {
        send_error(ERR_ROLE_HANDOFF_FAILED, CMD_AKAI_DISCOVER, "Akai discovery unavailable");
        return;
    }
    if (len != 2 || (payload[0] != 0xFF && payload[0] > 7) || payload[1] > 7) {
        send_error(ERR_INVALID_LENGTH, CMD_AKAI_DISCOVER, "Invalid discovery request");
        return;
    }
    if (payload[0] != 0xFF && payload[0] == payload[1]) {
        send_error(ERR_SCSI_ID_CONFLICT, CMD_AKAI_DISCOVER, "Duplicate SCSI IDs");
        return;
    }

    usb_bridge_akai_device_t device;
    memset(&device, 0, sizeof(device));
    if (!zbridge_akai_discover(payload[0], payload[1], &device)) {
        send_error(ERR_AKAI_NOT_FOUND, CMD_AKAI_DISCOVER, "Akai not found");
        return;
    }
    send_packet(RSP_AKAI_DEVICE, (const uint8_t*)&device, sizeof(device));
}

// =============================================================================
// Packet Processing
// =============================================================================

/**
 * Process a complete received packet.
 */
static void process_packet(void)
{
    // Packet format: sync(2) + length(2) + opcode(1) + payload + crc(2)
    // rx_buffer contains: length(2) + opcode(1) + payload
    // rx_length is opcode + payload length

    if (g_bridge.rx_received < 3) {
        // Too short
        return;
    }

    // Verify CRC
    uint16_t packet_crc = g_bridge.rx_buffer[g_bridge.rx_received - 2] |
                          (g_bridge.rx_buffer[g_bridge.rx_received - 1] << 8);
    // rx_buffer begins with the two length bytes; v3 CRC covers only the
    // opcode+payload body of rx_length bytes.
    uint16_t calc_crc = crc16_ccitt(g_bridge.rx_buffer + 2, g_bridge.rx_length);

    if (packet_crc != calc_crc) {
        send_error(ERR_CRC_MISMATCH, 0, "CRC verification failed");
        return;
    }

    // Extract opcode and payload
    uint8_t opcode = g_bridge.rx_buffer[2];
    const uint8_t* payload = g_bridge.rx_buffer + 3;
    uint16_t payload_len = g_bridge.rx_length - 1;  // Subtract opcode byte

    if (g_bridge.debug_enabled) {
        dbgmsg("USB Bridge: opcode=", opcode, " payload_len=", payload_len);
    }

    // Dispatch to handler
    switch (opcode) {
        case CMD_PING:
            handle_ping();
            break;
        case CMD_GET_INFO:
            handle_get_info();
            break;
        case CMD_READ_BLOCKS:
            handle_read_blocks(payload, payload_len);
            break;
        case CMD_WRITE_BLOCKS:
            handle_write_blocks(payload, payload_len);
            break;
        case CMD_SYNC_TO_SD:
            handle_sync_to_sd(payload, payload_len);
            break;
        case CMD_GET_DIRTY_BLOCKS:
            handle_get_dirty_blocks(payload, payload_len);
            break;
        case CMD_CLEAR_OVERLAY:
            handle_clear_overlay();
            break;
        case CMD_GET_SCSI_STATUS:
            handle_get_scsi_status();
            break;
        case CMD_SUBSCRIBE_EVENTS:
            handle_subscribe_events(payload, payload_len);
            break;
        case CMD_SET_ACTIVE_IMAGE:
            handle_set_active_image(payload, payload_len);
            break;
        case CMD_GET_OVERLAY_STATS:
            handle_get_overlay_stats();
            break;
        case CMD_READ_OVERLAY_ONLY:
            handle_read_overlay_only(payload, payload_len);
            break;
        case CMD_INVALIDATE_BLOCK:
            handle_invalidate_block(payload, payload_len);
            break;
        case CMD_MIDI_SEND_BATCH:
            handle_midi_send_batch(payload, payload_len);
            break;
        case CMD_MIDI_FLUSH:
            handle_midi_flush(payload, payload_len);
            break;
        case CMD_AKAI_DISCOVER:
            handle_akai_discover(payload, payload_len);
            break;
        default:
            send_error(ERR_INVALID_COMMAND, opcode, "Unknown command");
            break;
    }
}

// =============================================================================
// Public API
// =============================================================================

void usb_bridge_init(void)
{
    memset(&g_bridge, 0, sizeof(g_bridge));
    g_bridge.rx_state = RX_STATE_SYNC1;
    g_bridge.initialized = true;

    // Initialize RAM overlay
    ram_overlay_init();

    // Initialize CDC layer
    platform_cdc_init();

    logmsg("USB Bridge initialized");
}

int usb_bridge_poll(void)
{
    if (!g_bridge.initialized) {
        return 0;
    }

    // Process USB tasks
    platform_cdc_task();

    // Check connection state
    bool was_connected = g_bridge.connected;
    g_bridge.connected = platform_cdc_connected();

    if (was_connected && !g_bridge.connected) {
        // Disconnected - reset state
        g_bridge.rx_state = RX_STATE_SYNC1;
        g_bridge.event_mask = 0;
        dbgmsg("USB Bridge disconnected");
    } else if (!was_connected && g_bridge.connected) {
        dbgmsg("USB Bridge connected");
    }

    if (!g_bridge.connected) {
        return 0;
    }

    int packets_processed = 0;

    // Process incoming data
    while (platform_cdc_available() > 0) {
        uint8_t byte;
        if (!platform_cdc_read_byte(&byte)) {
            break;
        }

        switch (g_bridge.rx_state) {
            case RX_STATE_SYNC1:
                if (byte == 0xAA) {
                    g_bridge.rx_state = RX_STATE_SYNC2;
                }
                break;

            case RX_STATE_SYNC2:
                if (byte == 0x55) {
                    g_bridge.rx_state = RX_STATE_LENGTH_LOW;
                } else if (byte == 0xAA) {
                    // Stay in SYNC2
                } else {
                    g_bridge.rx_state = RX_STATE_SYNC1;
                }
                break;

            case RX_STATE_LENGTH_LOW:
                g_bridge.rx_buffer[0] = byte;
                g_bridge.rx_length = byte;
                g_bridge.rx_state = RX_STATE_LENGTH_HIGH;
                break;

            case RX_STATE_LENGTH_HIGH:
                g_bridge.rx_buffer[1] = byte;
                g_bridge.rx_length |= (uint16_t)byte << 8;

                if (g_bridge.rx_length == 0 || g_bridge.rx_length > USB_BRIDGE_MAX_PAYLOAD + 1) {
                    // Invalid length
                    g_bridge.rx_state = RX_STATE_SYNC1;
                } else {
                    g_bridge.rx_received = 2;  // We have length bytes
                    g_bridge.rx_state = RX_STATE_PAYLOAD;
                }
                break;

            case RX_STATE_PAYLOAD:
                g_bridge.rx_buffer[g_bridge.rx_received++] = byte;

                // Total expected: length(2) + opcode+payload(rx_length) + crc(2)
                uint16_t expected = 2 + g_bridge.rx_length + 2;

                if (g_bridge.rx_received >= expected) {
                    // Complete packet received
                    process_packet();
                    packets_processed++;
                    g_bridge.rx_state = RX_STATE_SYNC1;
                }
                break;
        }
    }

    return packets_processed;
}

bool usb_bridge_is_connected(void)
{
    return g_bridge.connected;
}

bool usb_bridge_send_notify_scsi_command(uint8_t cmd, uint32_t lba, uint16_t count)
{
    if (!g_bridge.connected) return false;
    if (!(g_bridge.event_mask & (EVENT_MASK_SCSI_READ | EVENT_MASK_SCSI_WRITE))) return false;

    // Track for status queries
    g_bridge.last_scsi_command = cmd;
    g_bridge.last_scsi_lba = lba;
    g_bridge.last_scsi_count = count;

    usb_bridge_scsi_notify_t notify;
    notify.command = cmd;
    notify.lba = lba;
    notify.block_count = count;
    notify.flags = 0;

    return send_packet(NOTIFY_SCSI_COMMAND, (uint8_t*)&notify, sizeof(notify));
}

bool usb_bridge_send_notify_blocks_written(uint32_t lba, uint16_t count)
{
    if (!g_bridge.connected) return false;
    if (!(g_bridge.event_mask & EVENT_MASK_SCSI_WRITE)) return false;

    usb_bridge_blocks_written_t notify;
    notify.start_lba = lba;
    notify.block_count = count;

    return send_packet(NOTIFY_BLOCKS_WRITTEN, (uint8_t*)&notify, sizeof(notify));
}

bool usb_bridge_send_notify_overlay_full(void)
{
    if (!g_bridge.connected) return false;
    if (!(g_bridge.event_mask & EVENT_MASK_OVERLAY_FULL)) return false;

    return send_packet(NOTIFY_OVERLAY_FULL, nullptr, 0);
}

bool usb_bridge_send_notify_sd_removed(void)
{
    if (!g_bridge.connected) return false;
    if (!(g_bridge.event_mask & EVENT_MASK_SD_CHANGES)) return false;

    return send_packet(NOTIFY_SD_REMOVED, nullptr, 0);
}

bool usb_bridge_send_notify_sd_inserted(void)
{
    if (!g_bridge.connected) return false;
    if (!(g_bridge.event_mask & EVENT_MASK_SD_CHANGES)) return false;

    return send_packet(NOTIFY_SD_INSERTED, nullptr, 0);
}

bool usb_bridge_send_notify_scsi_bus_reset(void)
{
    if (!g_bridge.connected) return false;
    if (!(g_bridge.event_mask & EVENT_MASK_SCSI_BUS_RESET)) return false;

    return send_packet(NOTIFY_SCSI_BUS_RESET, nullptr, 0);
}

bool usb_bridge_send_notify_midi(uint8_t port, uint32_t timestamp_us,
                                 const uint8_t* data, uint16_t length)
{
    if (!g_bridge.connected || !data || length == 0) return false;
    if (!(g_bridge.event_mask & EVENT_MASK_MIDI_INPUT)) return false;
    if (!zbridge_midi_input_available()) return false;
    if ((uint32_t)length + 10 > USB_BRIDGE_MAX_PAYLOAD) return false;

    uint8_t* payload = g_bridge.tx_buffer + 5;
    payload[0] = port;
    payload[1] = 0;
    payload[2] = 1; // event count, LE
    payload[3] = 0;
    payload[4] = timestamp_us & 0xFF;
    payload[5] = (timestamp_us >> 8) & 0xFF;
    payload[6] = (timestamp_us >> 16) & 0xFF;
    payload[7] = (timestamp_us >> 24) & 0xFF;
    payload[8] = length & 0xFF;
    payload[9] = (length >> 8) & 0xFF;
    memcpy(payload + 10, data, length);
    return send_packet(NOTIFY_MIDI_INPUT, payload, 10 + length);
}

uint16_t usb_bridge_get_event_mask(void)
{
    return g_bridge.event_mask;
}

void usb_bridge_set_debug(bool enabled)
{
    g_bridge.debug_enabled = enabled;
}
