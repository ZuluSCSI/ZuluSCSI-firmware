/**
 * ZuluSCSI USB Bridge Protocol Handler
 *
 * Enables direct USB communication between Mac and Akai samplers
 * via ZuluSCSI, eliminating SD card swapping.
 *
 * ZBridge Protocol Version: 3
 *
 * Copyright (c) 2025
 * Licensed under GPL-3.0
 */

#ifndef ZULUSCSI_USB_BRIDGE_H
#define ZULUSCSI_USB_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Protocol Constants
// =============================================================================

#define USB_BRIDGE_PROTOCOL_VERSION     3
#define USB_BRIDGE_SYNC_WORD            0x55AA  // Little-endian: 0xAA 0x55
#define USB_BRIDGE_MAX_PAYLOAD          4096
#define USB_BRIDGE_MAX_PACKET           4103    // 2 + 2 + 1 + 4096 + 2
#define USB_BRIDGE_BLOCK_SIZE           512
#define USB_BRIDGE_MAX_BLOCKS_PER_CMD   7

// Baud rate (logical, USB handles actual timing)
#define USB_BRIDGE_BAUD_RATE            921600

// =============================================================================
// Command Opcodes (Host -> Device)
// =============================================================================

typedef enum {
    CMD_PING                = 0x01,
    CMD_GET_INFO            = 0x02,
    CMD_READ_BLOCKS         = 0x03,
    CMD_WRITE_BLOCKS        = 0x04,
    CMD_SYNC_TO_SD          = 0x05,
    CMD_GET_DIRTY_BLOCKS    = 0x06,
    CMD_CLEAR_OVERLAY       = 0x07,
    CMD_GET_SCSI_STATUS     = 0x08,
    CMD_SUBSCRIBE_EVENTS    = 0x09,
    CMD_SET_ACTIVE_IMAGE    = 0x0A,
    CMD_GET_OVERLAY_STATS   = 0x0B,
    CMD_READ_OVERLAY_ONLY   = 0x0C,
    CMD_INVALIDATE_BLOCK    = 0x0D,
    CMD_LIST_FILES          = 0x20,
    CMD_FILE_OPEN_WRITE     = 0x21,
    CMD_FILE_WRITE_CHUNK    = 0x22,
    CMD_FILE_CLOSE_COMMIT   = 0x23,
    CMD_FILE_READ_OPEN      = 0x24,
    CMD_FILE_READ_CHUNK     = 0x25,
    CMD_EJECT_CYCLE         = 0x30,
    CMD_QUIESCE             = 0x31,
    CMD_BRIDGE_EXIT         = 0x3F,
    CMD_MIDI_SEND_BATCH     = 0x40,
    CMD_MIDI_FLUSH          = 0x41,
    CMD_AKAI_DISCOVER       = 0x50,
} usb_bridge_cmd_t;

// =============================================================================
// Response Opcodes (Device -> Host)
// =============================================================================

typedef enum {
    RSP_PONG                = 0x81,
    RSP_DEVICE_INFO         = 0x82,
    RSP_BLOCK_DATA          = 0x83,
    RSP_WRITE_ACK           = 0x84,
    RSP_SYNC_COMPLETE       = 0x85,
    RSP_DIRTY_BLOCK_LIST    = 0x86,
    RSP_SCSI_STATUS         = 0x87,
    RSP_OVERLAY_STATS       = 0x88,
    RSP_ERROR               = 0x8F,
    RSP_AKAI_DEVICE         = 0x90,
} usb_bridge_rsp_t;

// =============================================================================
// Notification Opcodes (Device -> Host, Async)
// =============================================================================

typedef enum {
    NOTIFY_SCSI_COMMAND     = 0xE1,
    NOTIFY_BLOCKS_WRITTEN   = 0xE2,
    NOTIFY_OVERLAY_FULL     = 0xE3,
    NOTIFY_SD_REMOVED       = 0xE4,
    NOTIFY_SD_INSERTED      = 0xE5,
    NOTIFY_SCSI_BUS_RESET   = 0xE6,
    NOTIFY_MIDI_INPUT       = 0xE7,
} usb_bridge_notify_t;

// =============================================================================
// Error Codes
// =============================================================================

typedef enum {
    ERR_NONE                = 0x00,
    ERR_INVALID_COMMAND     = 0x01,
    ERR_INVALID_LENGTH      = 0x02,
    ERR_CRC_MISMATCH        = 0x03,
    ERR_INVALID_LBA         = 0x04,
    ERR_BLOCK_COUNT         = 0x05,
    ERR_OVERLAY_FULL        = 0x06,
    ERR_SD_READ             = 0x07,
    ERR_SD_WRITE            = 0x08,
    ERR_NO_SD               = 0x09,
    ERR_SCSI_BUSY           = 0x0A,
    ERR_NO_IMAGE            = 0x0B,
    ERR_IMAGE_INDEX         = 0x0C,
    ERR_TIMEOUT             = 0x0D,
    ERR_INTERNAL            = 0x0E,
    ERR_FILE_NOT_FOUND      = 0x0F,
    ERR_FILE_STATE          = 0x10,
    ERR_MIDI_UNAVAILABLE    = 0x11,
    ERR_MIDI_QUEUE_FULL     = 0x12,
    ERR_INVALID_MIDI        = 0x13,
    ERR_AKAI_NOT_FOUND      = 0x14,
    ERR_SCSI_ID_CONFLICT    = 0x15,
    ERR_ROLE_HANDOFF_FAILED = 0x16,
} usb_bridge_error_t;

// =============================================================================
// Hardware Type
// =============================================================================

typedef enum {
    HW_TYPE_RP2040          = 0x01,
    HW_TYPE_RP2350B         = 0x02,
    HW_TYPE_RP2350A         = 0x03,
} usb_bridge_hw_type_t;

// =============================================================================
// SCSI Phase Values
// =============================================================================

typedef enum {
    SCSI_PHASE_BUS_FREE     = 0x00,
    SCSI_PHASE_ARBITRATION  = 0x01,
    SCSI_PHASE_SELECTION    = 0x02,
    SCSI_PHASE_RESELECTION  = 0x03,
    SCSI_PHASE_COMMAND      = 0x04,
    SCSI_PHASE_DATA_IN      = 0x05,
    SCSI_PHASE_DATA_OUT     = 0x06,
    SCSI_PHASE_STATUS       = 0x07,
    SCSI_PHASE_MESSAGE_IN   = 0x08,
    SCSI_PHASE_MESSAGE_OUT  = 0x09,
} usb_bridge_scsi_phase_t;

// =============================================================================
// Event Subscription Mask
// =============================================================================

#define EVENT_MASK_SCSI_READ        (1 << 0)
#define EVENT_MASK_SCSI_WRITE       (1 << 1)
#define EVENT_MASK_OVERLAY_FULL     (1 << 2)
#define EVENT_MASK_SD_CHANGES       (1 << 3)
#define EVENT_MASK_SCSI_BUS_RESET   (1 << 4)
#define EVENT_MASK_MIDI_INPUT       (1 << 5)

// =============================================================================
// Device Info Flags
// =============================================================================

#define FLAG_OVERLAY_SUPPORTED      (1 << 0)
#define FLAG_EVENTS_SUPPORTED       (1 << 1)
#define FLAG_MULTI_IMAGE            (1 << 2)
#define FLAG_WRITE_THROUGH          (1 << 3)
#define FLAG_FILE_TRANSFER          (1 << 4)
#define FLAG_MIDI_STREAM            (1 << 5)
#define FLAG_MIDI_INPUT             (1 << 6)
#define FLAG_AKAI_SCSI_DISCOVERY    (1 << 7)

// =============================================================================
// Sync Flags
// =============================================================================

#define SYNC_FLAG_CLEAR_AFTER       (1 << 0)
#define SYNC_FLAG_VERIFY            (1 << 1)

// =============================================================================
// Dirty Block Filter
// =============================================================================

typedef enum {
    DIRTY_FILTER_ALL        = 0x00,
    DIRTY_FILTER_MAC_ONLY   = 0x01,
    DIRTY_FILTER_AKAI_ONLY  = 0x02,
} usb_bridge_dirty_filter_t;

// =============================================================================
// Packet Structures
// =============================================================================

#pragma pack(push, 1)

// Packet header (before payload)
typedef struct {
    uint16_t sync;          // 0xAA55
    uint16_t length;        // OPCODE + PAYLOAD length
    uint8_t  opcode;
} usb_bridge_packet_header_t;

// RSP_DEVICE_INFO payload (64 bytes, ZBridge v3 wire contract)
typedef struct {
    uint32_t firmware_ver;      // LE bytes: Major, Minor, Patch, Build
    uint16_t protocol_ver;      // 3
    uint32_t capabilities;
    uint8_t  scsi_id;
    uint8_t  num_images;
    uint8_t  active_image;
    uint8_t  reserved;
    char     serial[16];
    char     label[32];
    uint8_t  padding[2];
} usb_bridge_device_info_t;

// CMD_READ_BLOCKS / CMD_READ_OVERLAY_ONLY request
typedef struct {
    uint32_t start_lba;
    uint16_t block_count;
} usb_bridge_read_request_t;

// RSP_BLOCK_DATA header (data follows)
typedef struct {
    uint32_t start_lba;
    uint16_t block_count;
    // Followed by block_count * 512 bytes of data
} usb_bridge_block_data_header_t;

// CMD_WRITE_BLOCKS header (data follows)
typedef struct {
    uint32_t start_lba;
    uint16_t block_count;
    // Followed by block_count * 512 bytes of data
} usb_bridge_write_request_t;

// RSP_WRITE_ACK
typedef struct {
    uint32_t start_lba;
    uint16_t block_count;
    uint16_t overlay_used;
} usb_bridge_write_ack_t;

// CMD_SYNC_TO_SD request
typedef struct {
    uint8_t flags;
} usb_bridge_sync_request_t;

// RSP_SYNC_COMPLETE
typedef struct {
    uint8_t  status;
    uint16_t blocks_written;
    uint16_t errors;
} usb_bridge_sync_result_t;

// CMD_GET_DIRTY_BLOCKS request
typedef struct {
    uint8_t  filter;
    uint16_t max_count;
} usb_bridge_dirty_request_t;

// RSP_DIRTY_BLOCK_LIST header
typedef struct {
    uint16_t total_count;
    uint16_t returned_count;
    // Followed by returned_count * 4 bytes (LBAs)
} usb_bridge_dirty_list_header_t;

// CMD_SUBSCRIBE_EVENTS request
typedef struct {
    uint16_t event_mask;
} usb_bridge_subscribe_request_t;

// CMD_SET_ACTIVE_IMAGE request
typedef struct {
    uint8_t image_index;
} usb_bridge_set_image_request_t;

// RSP_SCSI_STATUS (18 bytes)
typedef struct {
    uint8_t  phase;
    uint8_t  last_command;
    uint32_t last_lba;
    uint16_t last_count;
    uint8_t  bus_signals;
    uint8_t  transfer_active;
    uint32_t bytes_pending;
    uint16_t reserved;
} usb_bridge_scsi_status_t;

// RSP_OVERLAY_STATS
typedef struct {
    uint16_t capacity;
    uint16_t used;
    uint16_t mac_blocks;
    uint16_t akai_blocks;
    uint16_t dirty_blocks;
    uint32_t total_reads;
    uint32_t total_writes;
    uint32_t cache_hits;
} usb_bridge_overlay_stats_t;

// NOTIFY_SCSI_COMMAND
typedef struct {
    uint8_t  command;
    uint32_t lba;
    uint16_t block_count;
    uint8_t  flags;
} usb_bridge_scsi_notify_t;

// NOTIFY_BLOCKS_WRITTEN
typedef struct {
    uint32_t start_lba;
    uint16_t block_count;
} usb_bridge_blocks_written_t;

// RSP_ERROR
typedef struct {
    uint8_t error_code;
    uint8_t context;
    char    message[60];    // Null-terminated
} usb_bridge_error_response_t;

// CMD_INVALIDATE_BLOCK request
typedef struct {
    uint32_t lba;
} usb_bridge_invalidate_request_t;

// RSP_AKAI_DEVICE: normalized standard INQUIRY identity (32 bytes).
typedef struct {
    uint8_t target_id;
    uint8_t initiator_id;
    uint8_t peripheral_type;
    uint8_t ansi_version;
    char vendor[8];
    char product[16];
    char revision[4];
} usb_bridge_akai_device_t;

#pragma pack(pop)

// =============================================================================
// API Functions
// =============================================================================

/**
 * Initialize the USB bridge subsystem.
 * Call once during startup after USB CDC is initialized.
 */
void usb_bridge_init(void);

/**
 * Poll for incoming USB packets and process them.
 * Call frequently from main loop when SCSI bus is idle.
 * Returns number of packets processed.
 */
int usb_bridge_poll(void);

/**
 * Check if USB bridge is connected and active.
 */
bool usb_bridge_is_connected(void);

/**
 * Send a notification to the host.
 * Returns true if sent successfully.
 */
bool usb_bridge_send_notify_scsi_command(uint8_t cmd, uint32_t lba, uint16_t count);
bool usb_bridge_send_notify_blocks_written(uint32_t lba, uint16_t count);
bool usb_bridge_send_notify_overlay_full(void);
bool usb_bridge_send_notify_sd_removed(void);
bool usb_bridge_send_notify_sd_inserted(void);
bool usb_bridge_send_notify_scsi_bus_reset(void);
bool usb_bridge_send_notify_midi(uint8_t port, uint32_t timestamp_us,
                                 const uint8_t* data, uint16_t length);

/**
 * Optional board integration hooks for live MIDI. The default weak
 * implementations report unavailable. A board port (DIN UART, USB-MIDI, or
 * another explicit route) must override these before MIDI is advertised.
 */
bool zbridge_midi_available(void);
bool zbridge_midi_input_available(void);
bool zbridge_midi_write(uint8_t port, uint32_t timestamp_us,
                        const uint8_t* data, uint16_t length);
void zbridge_midi_flush(uint8_t port);

/**
 * Optional, fail-closed Akai discovery seam. A board integration may return
 * true from `zbridge_akai_discovery_available` only after it can serialize a
 * target->initiator->target PHY handoff and guarantee cleanup on every exit.
 * Discovery may issue TEST UNIT READY and INQUIRY only.
 */
bool zbridge_akai_discovery_available(void);
bool zbridge_akai_discover(uint8_t requested_target, uint8_t initiator_id,
                           usb_bridge_akai_device_t* result);

/**
 * Get current event subscription mask.
 */
uint16_t usb_bridge_get_event_mask(void);

/**
 * Enable/disable debug logging for USB bridge.
 */
void usb_bridge_set_debug(bool enabled);

#ifdef __cplusplus
}
#endif

#endif // ZULUSCSI_USB_BRIDGE_H
