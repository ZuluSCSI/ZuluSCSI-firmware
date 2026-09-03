/*
 * ZBridge integration for ZuluSCSI firmware
 * Copyright (c) 2026 Brendan Spear
 *
 * This file is free software: you may copy, redistribute and/or modify it
 * under the terms of the GNU General Public License, version 2 or later,
 * consistent with the upstream ZuluSCSI firmware distribution.
 *
 * ZuluSCSI is an independent third-party project. No affiliation or
 * endorsement is implied.
 */

#include "ZBridge.h"

#include <Arduino.h>
#include <CoreMutex.h>
#include <SdFat.h>
#include <USB.h>
#include <pico/unique_id.h>
#include <stdio.h>
#include <string.h>
#include <tusb.h>

#include "ZuluSCSI_disk.h"
#include "ZuluSCSI_initiator.h"
#include "ZuluSCSI_platform.h"

extern "C" {
#include <scsi.h>
#include <scsi2sd.h>
}

extern SdFs SD;

namespace {
constexpr uint8_t SYNC0 = 0xAA;
constexpr uint8_t SYNC1 = 0x55;
// Protocol v5+ deliberately amortizes each USB/SD/SCSI acknowledgement over
// a 40 KiB frame while remaining below the u16 wire-length ceiling.
constexpr uint16_t MAX_PAYLOAD = 40 * 1024;
// A bounded credit ring lets USB remain ahead while PIO/DMA drains to Akai.
constexpr uint16_t MAX_PIPELINED_PCM = 4 * 1024;
constexpr uint8_t BULK_SLOT_COUNT = 2;
constexpr uint32_t BINARY_IDLE_MS = 3000;
constexpr uint32_t DIRECT_IDLE_MS = 15000;

enum Command : uint8_t {
    CMD_PING = 0x01, CMD_GET_INFO = 0x02, CMD_SUBSCRIBE = 0x09,
    CMD_LIST_FILES = 0x20, CMD_FILE_OPEN_WRITE = 0x21,
    CMD_FILE_WRITE_CHUNK = 0x22, CMD_FILE_CLOSE_COMMIT = 0x23,
    CMD_FILE_READ_OPEN = 0x24, CMD_FILE_READ_CHUNK = 0x25,
    CMD_EJECT_CYCLE = 0x30, CMD_QUIESCE = 0x31, CMD_EXIT = 0x3F,
    CMD_REBOOT_BOOTLOADER = 0x32,
    CMD_MIDI_SEND_BATCH = 0x40, CMD_MIDI_FLUSH = 0x41,
    CMD_AKAI_DISCOVER = 0x50, CMD_AKAI_SESSION_BEGIN = 0x51,
    CMD_AKAI_SCSI_EXECUTE = 0x52, CMD_AKAI_SESSION_END = 0x53,
    CMD_AKAI_BULK_BEGIN = 0x54, CMD_AKAI_BULK_DATA = 0x55,
    CMD_AKAI_BULK_FINISH = 0x56, CMD_AKAI_BULK_ABORT = 0x57,
    CMD_USB_BULK_BENCH_BEGIN = 0x58, CMD_USB_BULK_BENCH_STATUS = 0x59,
    CMD_AKAI_BULK_READ_BEGIN = 0x5A, CMD_AKAI_BULK_READ_FINISH = 0x5B,
    CMD_AKAI_BULK_READ_ABORT = 0x5C,
};

enum Response : uint8_t {
    RSP_PONG = 0x81, RSP_INFO = 0x82, RSP_ACK = 0x84,
    RSP_ERROR = 0x8F, RSP_AKAI_DEVICE = 0x90,
};

enum Error : uint8_t {
    ERR_INVALID_COMMAND = 0x01, ERR_INVALID_LENGTH = 0x02,
    ERR_CRC = 0x03, ERR_SD_READ = 0x07, ERR_SD_WRITE = 0x08,
    ERR_NO_SD = 0x09, ERR_SCSI_BUSY = 0x0A, ERR_NO_IMAGE = 0x0B,
    ERR_IMAGE_INDEX = 0x0C, ERR_TIMEOUT = 0x0D, ERR_INTERNAL = 0x0E,
    ERR_FILE_NOT_FOUND = 0x0F, ERR_FILE_STATE = 0x10,
    ERR_MIDI_UNAVAILABLE = 0x11, ERR_MIDI_QUEUE_FULL = 0x12,
    ERR_INVALID_MIDI = 0x13,
    ERR_AKAI_NOT_FOUND = 0x14, ERR_SCSI_ID_CONFLICT = 0x15,
    ERR_ROLE_HANDOFF = 0x16, ERR_AKAI_SESSION_STATE = 0x17,
    ERR_SCSI_COMMAND = 0x18, ERR_DIRECT_BUSY = 0x19,
};

enum RxState { RX_SYNC0, RX_SYNC1, RX_LEN0, RX_LEN1, RX_BODY, RX_CRC0, RX_CRC1 };

enum InitiatorResult {
    INITIATOR_OK = 0,
    INITIATOR_BUS_NOT_FREE = -10,
    INITIATOR_NO_TARGET = -11,
    INITIATOR_PHASE_TIMEOUT = -12,
    INITIATOR_TRANSPORT_ERROR = -13,
    INITIATOR_NOT_AKAI = -14,
};

struct State {
    RxState rx = RX_SYNC0;
    uint16_t bodyLength = 0;
    uint16_t bodyUsed = 0;
    uint16_t wireCRC = 0;
    uint8_t body[MAX_PAYLOAD + 1];
    bool binary = false;
    bool quiesce = false;
    bool direct = false;
    uint8_t directTarget = 0xFF;
    uint32_t lastActivity = 0;
    uint32_t primaryTimeoutSignals = 0;
    uint32_t fallbackTimeoutSignals = 0;
    int8_t primaryTimeoutPhase = BUS_FREE;
    int8_t fallbackTimeoutPhase = BUS_FREE;
    uint8_t primarySelectionStage = 0;
    uint8_t fallbackSelectionStage = 0;
    uint8_t primaryPhaseHistory = 0;
    uint8_t fallbackPhaseHistory = 0;
    uint32_t primaryGPIOState = 0;
    uint32_t fallbackGPIOState = 0;
    FsFile file;
    bool writing = false;
    bool reading = false;
    uint64_t expectedSize = 0;
    char finalName[32] = {};
    char partName[40] = {};
} g;

struct BulkSlot {
    uint32_t sequence = 0;
    uint32_t acceptedBytes = 0;
    uint16_t length = 0;
    bool occupied = false;
    uint8_t data[MAX_PIPELINED_PCM];
};

struct BulkStreamState {
    bool active = false;
    bool vendorTransport = false;
    bool draining = false;
    bool commandRunning = false;
    bool commandComplete = false;
    bool finishPending = false;
    bool abortRequested = false;
    bool failed = false;
    bool consumedFirstByte = false;
    uint32_t expectedBytes = 0;
    uint32_t expectedChunks = 0;
    uint32_t nextSequence = 0;
    uint32_t acceptedBytes = 0;
    uint32_t consumedBytes = 0;
    uint32_t sentBytes = 0;
    uint32_t commandMicros = 0;
    uint32_t startupWaitMicros = 0;
    uint32_t starvationMicros = 0;
    uint32_t starvationEvents = 0;
    uint8_t head = 0;
    uint8_t tail = 0;
    uint8_t count = 0;
    uint16_t headOffset = 0;
    char failure[64] = {};
    BulkSlot slots[BULK_SLOT_COUNT];
} g_bulk;

struct USBBulkBenchmarkState {
    bool active = false;
    bool complete = false;
    uint32_t expectedBytes = 0;
    uint32_t receivedBytes = 0;
    uint32_t startedMicros = 0;
    uint32_t elapsedMicros = 0;
    uint32_t checksum = 2166136261u;
} g_usbBenchmark;

struct BulkReadStreamState {
    bool active = false;
    bool commandRunning = false;
    bool commandComplete = false;
    bool finishPending = false;
    bool abortRequested = false;
    bool failed = false;
    bool sentFirstByte = false;
    uint32_t expectedBytes = 0;
    uint32_t sentBytes = 0;
    uint32_t commandMicros = 0;
    uint32_t startupWaitMicros = 0;
    uint32_t starvationMicros = 0;
    uint32_t starvationEvents = 0;
    uint32_t checksum = 2166136261u;
    uint32_t lastProgressMillis = 0;
    char failure[64] = {};
} g_bulkRead;

uint8_t g_vendorInterfaceID = 0;
uint8_t g_vendorEndpointIn = 0;
uint8_t g_vendorEndpointOut = 0;
uint8_t g_vendorStringID = 0;

void vendorInterfaceDescriptor(int interfaceNumber, uint8_t *destination,
                               int length, void *) {
    uint8_t descriptor[TUD_VENDOR_DESC_LEN] = {
        TUD_VENDOR_DESCRIPTOR(uint8_t(interfaceNumber), g_vendorStringID,
                              g_vendorEndpointOut, g_vendorEndpointIn,
                              CFG_TUD_VENDOR_EPSIZE)
    };
    if (length > int(sizeof(descriptor))) length = sizeof(descriptor);
    memcpy(destination, descriptor, size_t(length));
}

void resetBulkStream() { g_bulk = BulkStreamState{}; }
void resetBulkReadStream() { g_bulkRead = BulkReadStreamState{}; }

void pumpVendorBenchmark() {
    if (!g_usbBenchmark.active) return;

    uint8_t input[1024];
    while (g_usbBenchmark.receivedBytes < g_usbBenchmark.expectedBytes) {
        uint32_t received = 0;
        {
            CoreMutex mutex(&USB.mutex, false);
            if (!mutex) break;
            tud_task();
            uint32_t available = tud_vendor_available();
            if (!available) break;
            uint32_t remaining = g_usbBenchmark.expectedBytes
                - g_usbBenchmark.receivedBytes;
            uint32_t requested = available;
            if (requested > sizeof(input)) requested = sizeof(input);
            if (requested > remaining) requested = remaining;
            received = tud_vendor_read(input, requested);
        }
        if (!received) break;
        for (uint32_t index = 0; index < received; ++index) {
            g_usbBenchmark.checksum ^= input[index];
            g_usbBenchmark.checksum *= 16777619u;
        }
        g_usbBenchmark.receivedBytes += received;
        g.lastActivity = millis();
    }

    if (g_usbBenchmark.receivedBytes == g_usbBenchmark.expectedBytes) {
        g_usbBenchmark.elapsedMicros = uint32_t(
            micros() - g_usbBenchmark.startedMicros);
        g_usbBenchmark.active = false;
        g_usbBenchmark.complete = true;
    }
}

constexpr uint16_t CRC16_NIBBLE[16] = {
    0x0000, 0x1021, 0x2042, 0x3063,
    0x4084, 0x50A5, 0x60C6, 0x70E7,
    0x8108, 0x9129, 0xA14A, 0xB16B,
    0xC18C, 0xD1AD, 0xE1CE, 0xF1EF,
};

uint16_t crc16Update(uint16_t crc, const uint8_t *data, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        uint8_t index = uint8_t((crc >> 12) ^ (data[i] >> 4));
        crc = uint16_t((crc << 4) ^ CRC16_NIBBLE[index & 0x0F]);
        index = uint8_t((crc >> 12) ^ (data[i] & 0x0F));
        crc = uint16_t((crc << 4) ^ CRC16_NIBBLE[index & 0x0F]);
    }
    return crc;
}

uint16_t crc16(const uint8_t *data, size_t length) {
    return crc16Update(0xFFFF, data, length);
}

uint16_t read16(const uint8_t *p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
uint32_t read32(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint64_t read64(const uint8_t *p) { return uint64_t(read32(p)) | (uint64_t(read32(p + 4)) << 32); }
void write16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
void write32(uint8_t *p, uint32_t v) {
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
void write64(uint8_t *p, uint64_t v) { write32(p, v); write32(p + 4, v >> 32); }

void resetParser() { g.rx = RX_SYNC0; g.bodyLength = g.bodyUsed = g.wireCRC = 0; }

bool sendFrame(uint8_t opcode, const uint8_t *payload = nullptr, uint16_t payloadLength = 0) {
    if (payloadLength > MAX_PAYLOAD) return false;
    uint16_t length = payloadLength + 1;
    uint8_t header[5] = {SYNC0, SYNC1, uint8_t(length), uint8_t(length >> 8), opcode};
    uint16_t crc = crc16Update(0xFFFF, &opcode, 1);
    if (payloadLength) crc = crc16Update(crc, payload, payloadLength);
    uint8_t trailer[2] = {uint8_t(crc), uint8_t(crc >> 8)};
    size_t written = Serial.write(header, sizeof(header));
    if (payloadLength) written += Serial.write(payload, payloadLength);
    written += Serial.write(trailer, sizeof(trailer));
    Serial.flush();
    return written == payloadLength + 7;
}

void sendError(Error code, uint8_t context, const char *message) {
    uint8_t payload[66] = {};
    payload[0] = code; payload[1] = context;
    if (message) strncpy(reinterpret_cast<char *>(payload + 2), message, 63);
    sendFrame(RSP_ERROR, payload, sizeof(payload));
}

void sendAck(const uint8_t *body = nullptr, uint16_t bodyLength = 0) {
    if (bodyLength + 1 > MAX_PAYLOAD) { sendError(ERR_INTERNAL, 0, "ack too large"); return; }
    if (bodyLength) {
        if (body == g.body) memmove(g.body + 1, g.body, bodyLength);
        else memcpy(g.body + 1, body, bodyLength);
    }
    g.body[0] = 0;
    sendFrame(RSP_ACK, g.body, bodyLength + 1);
}

bool validName(const uint8_t *field, char out[32]) {
    size_t n = strnlen(reinterpret_cast<const char *>(field), 32);
    if (n == 0 || n >= 32) return false;
    for (size_t i = 0; i < n; ++i)
        if (field[i] == '/' || field[i] == '\\') return false;
    memcpy(out, field, n); out[n] = '\0';
    return true;
}

int activeDiskID() {
    for (int id = 0; id < S2S_MAX_TARGETS; ++id) {
        image_config_t &img = scsiDiskGetImageConfig(id);
        if (img.file.isOpen() && (img.scsiId & S2S_CFG_TARGET_ENABLED)) return id;
    }
    return 0;
}

void fillInquiryIdentity(const uint8_t inquiry[36], uint8_t target, uint8_t initiator,
                         uint8_t out[32]) {
    memset(out, 0, 32);
    out[0] = target; out[1] = initiator; out[2] = inquiry[0] & 0x1F; out[3] = inquiry[2];
    memcpy(out + 4, inquiry + 8, 8); memcpy(out + 12, inquiry + 16, 16); memcpy(out + 28, inquiry + 32, 4);
}

bool inquiryLooksAkai(const uint8_t inquiry[36]) {
    if ((inquiry[0] & 0x1F) != 0x03) return false;
    char identity[25] = {};
    memcpy(identity, inquiry + 8, 24);
    for (char &c : identity) if (c >= 'a' && c <= 'z') c -= 32;
    return strstr(identity, "AKAI") != nullptr;
}

void restoreTargetMode() {
    resetBulkStream();
    resetBulkReadStream();
    if (!g.direct) return;
    scsiHostPhyRelease();
#if defined(ZBRIDGE_NATIVE_DIAGNOSTIC) || defined(ZBRIDGE_DIRECT_RAM_MODE)
    // Remain in the platform's native initiator configuration. The normal
    // imaging loop is disabled for this diagnostic build.
    g.direct = false;
    g.directTarget = 0xFF;
    g.quiesce = false;
    return;
#endif
    scsiHostPhyDeactivate();
    scsiPhyReset();
    scsiInit();
    g.direct = false;
    g.directTarget = 0xFF;
    g.quiesce = false;
}

int enterInitiator(uint8_t target, uint8_t initiator, uint8_t inquiry[36]) {
    if (target > 7 || initiator > 7 || target == initiator) return INITIATOR_TRANSPORT_ERROR;
    if (scsiDev.phase != BUS_FREE || g.direct) return INITIATOR_BUS_NOT_FREE;
    g.quiesce = true;
#if !defined(ZBRIDGE_NATIVE_DIAGNOSTIC) && !defined(ZBRIDGE_DIRECT_RAM_MODE)
    scsiPhyDisable();
    // A live ZBridge session begins in disk-target mode. Reconfigure every
    // shared pin and transceiver direction for initiator mode. Do not reset
    // the entire SCSI bus here: MESA discovers an already-running sampler
    // without resetting it, and a reset makes the Akai immediately resume its
    // own disk-initiator polling before processor selection.
    scsiHostPhyActivateNoReset();
    delayMicroseconds(10);
#else
    // platform_late_init() and scsiInitiatorInit() already configured and
    // reset the PHY as a real initiator. Do not perform a live role switch.
    scsiHostPhyRelease();
#endif
    scsiInitiatorSetOwnID(initiator);
    g.primaryTimeoutSignals = 0;
    g.fallbackTimeoutSignals = 0;
    g.primaryTimeoutPhase = BUS_FREE;
    g.fallbackTimeoutPhase = BUS_FREE;
    g.primarySelectionStage = 0;
    g.fallbackSelectionStage = 0;
    g.primaryPhaseHistory = 0;
    g.fallbackPhaseHistory = 0;
    g.primaryGPIOState = 0;
    g.fallbackGPIOState = 0;
    memset(inquiry, 0, 36);
    int status = scsiInitiatorRunCommand(target,
        reinterpret_cast<const uint8_t *>("\x12\0\0\0\x24\0"), 6,
        inquiry, 36, nullptr, 0, false, 1500, true, false);

    // Prefer the Blaster's proven upstream selection sequence, which presents
    // both target and initiator IDs without ATN. If that fails, retry with the
    // standards-style ATN + IDENTIFY sequence used by MESA/SCSI2Pi.
    if (status == -4 || status == -1) {
        g.primaryTimeoutSignals = g_scsiInitiatorTimeoutSignals;
        g.primaryTimeoutPhase = g_scsiInitiatorTimeoutPhase;
        g.primarySelectionStage = g_scsiInitiatorTimeoutSelectionStage;
        g.primaryPhaseHistory = g_scsiInitiatorPhaseHistory;
        g.primaryGPIOState = g_scsiInitiatorTimeoutGPIOState;
        if (status == -4) {
            // SCSI reset recovery is slow on some S3000-series revisions.
            delay(750);
        } else {
            // Selection failure already released the bus. A short settle is
            // enough before retrying without ATN; avoid an unnecessary reset.
            delay(100);
        }
        memset(inquiry, 0, 36);
        // Retry once with standards-style ATN/IDENTIFY.
        status = scsiInitiatorRunCommand(target,
            reinterpret_cast<const uint8_t *>("\x12\0\0\0\x24\0"), 6,
            inquiry, 36, nullptr, 0, false, 4000, true, true);
        if (status == -4 || status == -1) {
            g.fallbackTimeoutSignals = g_scsiInitiatorTimeoutSignals;
            g.fallbackTimeoutPhase = g_scsiInitiatorTimeoutPhase;
            g.fallbackSelectionStage = g_scsiInitiatorTimeoutSelectionStage;
            g.fallbackPhaseHistory = g_scsiInitiatorPhaseHistory;
            g.fallbackGPIOState = g_scsiInitiatorTimeoutGPIOState;
        }
    }

    if (status != 0 || !inquiryLooksAkai(inquiry)) {
        g.direct = true; // make restoreTargetMode perform restoration
        restoreTargetMode();
        if (status == -4) return INITIATOR_PHASE_TIMEOUT;
        if (status == -1) return INITIATOR_NO_TARGET;
        if (status != 0) return INITIATOR_TRANSPORT_ERROR;
        return INITIATOR_NOT_AKAI;
    }
    g.direct = true;
    g.directTarget = target;
    g.lastActivity = millis();
    return INITIATOR_OK;
}

void closeTransferFile() {
    if (g.file.isOpen()) g.file.close();
    g.writing = g.reading = false;
}

void handleGetInfo() {
    uint8_t info[64] = {};
    write32(info, 0x34000034); // 52.0.0 build 52: native continuous DATA IN
    write16(info + 4, 9);
    write32(info + 6, (1u << 4) | (1u << 5) | (1u << 7) | (1u << 8) | (1u << 9)
                           | (1u << 10) | (1u << 11) | (1u << 12)
                           | (1u << 13) | (1u << 14) | (1u << 15)
                           | (1u << 16));
    info[10] = activeDiskID(); info[11] = 1; info[12] = 0;
    pico_unique_board_id_t uid; pico_get_unique_board_id(&uid);
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 8; ++i) {
        info[14 + i * 2] = hex[uid.id[i] >> 4];
        info[15 + i * 2] = hex[uid.id[i] & 0x0F];
    }
    memcpy(info + 30, "ZBridge", 7);
    sendFrame(RSP_INFO, info, sizeof(info));
}

void handleList(const uint8_t *payload, uint16_t length) {
    if (length != 2) { sendError(ERR_INVALID_LENGTH, CMD_LIST_FILES, "want u16 start"); return; }
    uint16_t start = read16(payload), total = 0, emitted = 0;
    FsFile root = SD.open("/", O_RDONLY), entry;
    if (!root.isOpen()) { sendError(ERR_NO_SD, CMD_LIST_FILES, "SD root unavailable"); return; }
    while (entry.openNext(&root, O_RDONLY)) { if (!entry.isHidden()) ++total; entry.close(); }
    root.rewindDirectory();
    uint8_t *out = g.body; write16(out, total); write16(out + 2, 0);
    uint16_t seen = 0;
    while (entry.openNext(&root, O_RDONLY)) {
        if (entry.isHidden()) { entry.close(); continue; }
        if (seen++ < start) { entry.close(); continue; }
        if (4 + (emitted + 1) * 41 > MAX_PAYLOAD - 1) { entry.close(); break; }
        uint8_t *item = out + 4 + emitted * 41; memset(item, 0, 41);
        entry.getName(reinterpret_cast<char *>(item), 32);
        write64(item + 32, entry.fileSize()); item[40] = entry.isDir() ? 1 : 0;
        ++emitted; entry.close();
    }
    root.close(); write16(out + 2, emitted); sendAck(out, 4 + emitted * 41);
}

void handleOpenWrite(const uint8_t *payload, uint16_t length) {
    if (length != 40) { sendError(ERR_INVALID_LENGTH, CMD_FILE_OPEN_WRITE, "want name+u64"); return; }
    closeTransferFile();
    if (!validName(payload, g.finalName)) { sendError(ERR_FILE_STATE, CMD_FILE_OPEN_WRITE, "bad filename"); return; }
    snprintf(g.partName, sizeof(g.partName), "%s.part", g.finalName);
    if (SD.exists(g.partName)) SD.remove(g.partName);
    g.file = SD.open(g.partName, O_CREAT | O_TRUNC | O_RDWR);
    if (!g.file.isOpen()) { sendError(ERR_SD_WRITE, CMD_FILE_OPEN_WRITE, "open part failed"); return; }
    g.expectedSize = read64(payload + 32); g.writing = true; sendAck();
}

void handleWrite(const uint8_t *payload, uint16_t length) {
    if (!g.writing || !g.file.isOpen()) { sendError(ERR_FILE_STATE, CMD_FILE_WRITE_CHUNK, "no write open"); return; }
    if (length < 5) { sendError(ERR_INVALID_LENGTH, CMD_FILE_WRITE_CHUNK, "want offset+data"); return; }
    uint32_t offset = read32(payload), count = length - 4;
    if (uint64_t(offset) + count > g.expectedSize || !g.file.seekSet(offset)) {
        sendError(ERR_FILE_STATE, CMD_FILE_WRITE_CHUNK, "bad offset"); return;
    }
    if (g.file.write(payload + 4, count) != int(count)) {
        sendError(ERR_SD_WRITE, CMD_FILE_WRITE_CHUNK, "chunk write failed"); return;
    }
    sendAck();
}

void handleCommit() {
    if (!g.writing || !g.file.isOpen()) { sendError(ERR_FILE_STATE, CMD_FILE_CLOSE_COMMIT, "no write open"); return; }
    g.file.flush(); uint64_t actual = g.file.fileSize(); g.file.close(); g.writing = false;
    if (actual != g.expectedSize) { SD.remove(g.partName); sendError(ERR_SD_WRITE, CMD_FILE_CLOSE_COMMIT, "size mismatch"); return; }
    if (SD.exists(g.finalName) && !SD.remove(g.finalName)) { sendError(ERR_SD_WRITE, CMD_FILE_CLOSE_COMMIT, "replace failed"); return; }
    FsFile part = SD.open(g.partName, O_RDWR);
    bool renamed = part.isOpen() && part.rename(g.finalName); part.close();
    if (!renamed) { sendError(ERR_SD_WRITE, CMD_FILE_CLOSE_COMMIT, "commit rename failed"); return; }
    sendAck();
}

void handleOpenRead(const uint8_t *payload, uint16_t length) {
    if (length != 32) { sendError(ERR_INVALID_LENGTH, CMD_FILE_READ_OPEN, "want name"); return; }
    char name[32]; closeTransferFile();
    if (!validName(payload, name)) { sendError(ERR_FILE_STATE, CMD_FILE_READ_OPEN, "bad filename"); return; }
    g.file = SD.open(name, O_RDONLY);
    if (!g.file.isOpen()) { sendError(ERR_FILE_NOT_FOUND, CMD_FILE_READ_OPEN, "not found"); return; }
    g.reading = true; uint8_t size[8]; write64(size, g.file.fileSize()); sendAck(size, 8);
}

void handleRead(const uint8_t *payload, uint16_t length) {
    if (!g.reading || !g.file.isOpen()) { sendError(ERR_FILE_STATE, CMD_FILE_READ_CHUNK, "no read open"); return; }
    if (length != 6) { sendError(ERR_INVALID_LENGTH, CMD_FILE_READ_CHUNK, "want offset+length"); return; }
    uint32_t offset = read32(payload); uint16_t requested = read16(payload + 4);
    if (!requested || requested > 4087 || !g.file.seekSet(offset)) { sendError(ERR_FILE_STATE, CMD_FILE_READ_CHUNK, "bad read"); return; }
    uint8_t *out = g.body; write32(out, offset);
    int got = g.file.read(out + 6, requested);
    if (got <= 0) { sendError(ERR_SD_READ, CMD_FILE_READ_CHUNK, "read failed"); return; }
    write16(out + 4, got); sendAck(out, got + 6);
}

void handleDiscovery(const uint8_t *payload, uint16_t length) {
    if (length != 2) { sendError(ERR_INVALID_LENGTH, CMD_AKAI_DISCOVER, "want target+initiator"); return; }
    uint8_t requested = payload[0], initiator = payload[1];
    if (initiator > 7 || (requested <= 7 && requested == initiator)) { sendError(ERR_SCSI_ID_CONFLICT, CMD_AKAI_DISCOVER, "SCSI ID conflict"); return; }
    uint8_t inquiry[36], first = requested == 0xFF ? 0 : requested, last = requested == 0xFF ? 7 : requested;
    int lastResult = INITIATOR_NO_TARGET;
    for (uint8_t target = first; target <= last; ++target) {
        if (target == initiator) continue;
        int result = enterInitiator(target, initiator, inquiry);
        if (result == INITIATOR_OK) {
            uint8_t identity[32]; fillInquiryIdentity(inquiry, target, initiator, identity);
            restoreTargetMode(); sendFrame(RSP_AKAI_DEVICE, identity, sizeof(identity)); return;
        }
        if (result != INITIATOR_NO_TARGET) lastResult = result;
    }
    if (lastResult == INITIATOR_BUS_NOT_FREE) { sendError(ERR_SCSI_BUSY, CMD_AKAI_DISCOVER, "SCSI bus was not free"); return; }
    if (lastResult == INITIATOR_PHASE_TIMEOUT) {
        char detail[64];
        snprintf(detail, sizeof(detail), "A s%u p%d h%02X x%06lX g%07lX;N s%u p%d h%02X x%06lX g%07lX",
                 g.primarySelectionStage, g.primaryTimeoutPhase, g.primaryPhaseHistory,
                 (unsigned long)g.primaryTimeoutSignals,
                 (unsigned long)g.primaryGPIOState,
                 g.fallbackSelectionStage, g.fallbackTimeoutPhase, g.fallbackPhaseHistory,
                 (unsigned long)g.fallbackTimeoutSignals,
                 (unsigned long)g.fallbackGPIOState);
        sendError(ERR_TIMEOUT, CMD_AKAI_DISCOVER, detail); return;
    }
    if (lastResult == INITIATOR_TRANSPORT_ERROR) { sendError(ERR_SCSI_COMMAND, CMD_AKAI_DISCOVER, "SCSI inquiry transport failed"); return; }
    if (lastResult == INITIATOR_NOT_AKAI) { sendError(ERR_AKAI_NOT_FOUND, CMD_AKAI_DISCOVER, "SCSI target is not an Akai processor"); return; }
    char detail[64];
    snprintf(detail, sizeof(detail), "no BSY; S s%u x%06lX; L s%u x%06lX",
             g.primarySelectionStage, (unsigned long)g.primaryTimeoutSignals,
             g.fallbackSelectionStage, (unsigned long)g.fallbackTimeoutSignals);
    sendError(ERR_AKAI_NOT_FOUND, CMD_AKAI_DISCOVER, detail);
}

void handleSessionBegin(const uint8_t *payload, uint16_t length) {
    if (length != 2) { sendError(ERR_INVALID_LENGTH, CMD_AKAI_SESSION_BEGIN, "want target+initiator"); return; }
    if (g.direct) { sendError(ERR_DIRECT_BUSY, CMD_AKAI_SESSION_BEGIN, "session already active"); return; }
    if (payload[0] == payload[1]) { sendError(ERR_SCSI_ID_CONFLICT, CMD_AKAI_SESSION_BEGIN, "SCSI ID conflict"); return; }
    uint8_t inquiry[36];
    int result = enterInitiator(payload[0], payload[1], inquiry);
    if (result != INITIATOR_OK) {
        if (result == INITIATOR_BUS_NOT_FREE) sendError(ERR_SCSI_BUSY, CMD_AKAI_SESSION_BEGIN, "SCSI bus was not free");
        else if (result == INITIATOR_PHASE_TIMEOUT) {
            char detail[64];
            snprintf(detail, sizeof(detail), "A s%u p%d h%02X x%06lX g%07lX;N s%u p%d h%02X x%06lX g%07lX",
                     g.primarySelectionStage, g.primaryTimeoutPhase, g.primaryPhaseHistory,
                     (unsigned long)g.primaryTimeoutSignals,
                     (unsigned long)g.primaryGPIOState,
                     g.fallbackSelectionStage, g.fallbackTimeoutPhase, g.fallbackPhaseHistory,
                     (unsigned long)g.fallbackTimeoutSignals,
                     (unsigned long)g.fallbackGPIOState);
            sendError(ERR_TIMEOUT, CMD_AKAI_SESSION_BEGIN, detail);
        }
        else if (result == INITIATOR_NO_TARGET) sendError(ERR_AKAI_NOT_FOUND, CMD_AKAI_SESSION_BEGIN, "no response from Akai target ID");
        else if (result == INITIATOR_NOT_AKAI) sendError(ERR_AKAI_NOT_FOUND, CMD_AKAI_SESSION_BEGIN, "SCSI target is not an Akai processor");
        else sendError(ERR_ROLE_HANDOFF, CMD_AKAI_SESSION_BEGIN, "initiator handoff failed");
        return;
    }
    sendAck(inquiry, sizeof(inquiry));
}

void handleSCSIExecute(const uint8_t *payload, uint16_t length) {
    if (!g.direct) { sendError(ERR_AKAI_SESSION_STATE, CMD_AKAI_SCSI_EXECUTE, "no active session"); return; }
    if (length < 13) { sendError(ERR_INVALID_LENGTH, CMD_AKAI_SCSI_EXECUTE, "short SCSI request"); return; }
    uint8_t direction = payload[0], cdbLength = payload[1];
    uint16_t timeout = read16(payload + 2); uint32_t readLength = read32(payload + 4), outLength = read32(payload + 8);
    if (!cdbLength || cdbLength > 16 || uint32_t(12 + cdbLength) + outLength != length ||
        readLength > MAX_PAYLOAD - 9 || direction > 2 || (direction == 0 && (readLength || outLength)) ||
        (direction == 1 && (!outLength || readLength)) || (direction == 2 && (outLength || !readLength))) {
        sendError(ERR_INVALID_LENGTH, CMD_AKAI_SCSI_EXECUTE, "invalid SCSI request"); return;
    }
    uint8_t cdb[16]; memcpy(cdb, payload + 12, cdbLength);
    const uint8_t *out = payload + 12 + cdbLength;
    uint8_t *in = direction == 2 ? g.body + 8 : nullptr;
    int status = scsiInitiatorRunCommand(g.directTarget, cdb, cdbLength,
        in, direction == 2 ? readLength : 0, direction == 1 ? out : nullptr,
        direction == 1 ? outLength : 0, false, timeout ? timeout : 10000, true, true);
    if (status == -4) { restoreTargetMode(); sendError(ERR_TIMEOUT, CMD_AKAI_SCSI_EXECUTE, "SCSI command phase timed out"); return; }
    if (status < 0) { sendError(ERR_SCSI_COMMAND, CMD_AKAI_SCSI_EXECUTE, "SCSI transport failed"); return; }
    uint8_t sense = 0, asc = 0, ascq = 0;
    if (status == 2) {
        uint8_t senseData[18] = {};
        uint8_t senseCDB[6] = {0x03, 0x00, 0x00, 0x00, sizeof(senseData), 0x00};
        int senseStatus = scsiInitiatorRunCommand(g.directTarget, senseCDB, sizeof(senseCDB),
            senseData, sizeof(senseData), nullptr, 0, false, 3000, true, true);
        if (senseStatus == -4) { restoreTargetMode(); sendError(ERR_TIMEOUT, CMD_AKAI_SCSI_EXECUTE, "SCSI request-sense phase timed out"); return; }
        if (senseStatus == 0) { sense = senseData[2] & 0x0F; asc = senseData[12]; ascq = senseData[13]; }
    }
    g.body[0] = status; g.body[1] = sense; g.body[2] = asc; g.body[3] = ascq;
    write32(g.body + 4, direction == 2 ? readLength : 0);
    g.lastActivity = millis(); sendAck(g.body, 8 + (direction == 2 ? readLength : 0));
}

// Forward one host MIDI batch through the Akai processor's MIDI-via-SCSI
// channel. Each event stays an independent SEND(6), preserving complete SysEx
// boundaries and timestamp offsets while keeping live note/CC packets on the
// shortest path (no TEST UNIT READY or generic SCSI envelope per event).
__attribute__((noinline))
void handleMIDISendBatch(const uint8_t *payload, uint16_t length) {
    if (!g.direct) {
        sendError(ERR_MIDI_UNAVAILABLE, CMD_MIDI_SEND_BATCH,
                  "start an Akai live MIDI session first");
        return;
    }
    if (g_bulk.active || g_bulkRead.active) {
        sendError(ERR_DIRECT_BUSY, CMD_MIDI_SEND_BATCH,
                  "sample stream owns the SCSI bus");
        return;
    }
    if (length < 4 || payload[0] != 0 || (payload[1] & ~uint8_t(0x03))) {
        sendError(ERR_INVALID_MIDI, CMD_MIDI_SEND_BATCH,
                  "invalid MIDI port or flags");
        return;
    }

    const uint8_t flags = payload[1];
    const uint16_t count = read16(payload + 2);
    if (!count) {
        sendError(ERR_INVALID_MIDI, CMD_MIDI_SEND_BATCH,
                  "empty MIDI batch");
        return;
    }

    // Validate the complete batch before transmitting its first event. A
    // malformed tail must never leave a partially applied live performance.
    uint32_t offset = 4;
    for (uint16_t index = 0; index < count; ++index) {
        if (offset + 6u > length) {
            sendError(ERR_INVALID_MIDI, CMD_MIDI_SEND_BATCH,
                      "truncated MIDI event header");
            return;
        }
        const uint32_t timestamp = read32(payload + offset);
        const uint16_t eventLength = read16(payload + offset + 4);
        offset += 6;
        if (!eventLength || offset + eventLength > length
            || timestamp > 5000000u) {
            sendError(ERR_INVALID_MIDI, CMD_MIDI_SEND_BATCH,
                      "invalid MIDI event length or timestamp");
            return;
        }
        offset += eventLength;
    }
    if (offset != length) {
        sendError(ERR_INVALID_MIDI, CMD_MIDI_SEND_BATCH,
                  "trailing MIDI bytes");
        return;
    }

    const uint32_t started = micros();
    offset = 4;
    uint16_t accepted = 0;
    for (uint16_t index = 0; index < count; ++index) {
        const uint32_t timestamp = read32(payload + offset);
        const uint16_t eventLength = read16(payload + offset + 4);
        const uint8_t *event = payload + offset + 6;
        offset += 6u + eventLength;

        if (!(flags & 0x01)) {
            while (uint32_t(micros() - started) < timestamp) {
                platform_reset_watchdog();
                tight_loop_contents();
            }
        }

        const uint8_t cdb[6] = {
            0x0C, 0x00,
            uint8_t(eventLength >> 16), uint8_t(eventLength >> 8),
            uint8_t(eventLength), 0x00,
        };
        const int status = scsiInitiatorRunCommand(
            g.directTarget, cdb, sizeof(cdb), nullptr, 0,
            event, eventLength, false, 3000, true, true);
        if (status == -4) {
            restoreTargetMode();
            sendError(ERR_TIMEOUT, CMD_MIDI_SEND_BATCH,
                      "Akai live MIDI command timed out");
            return;
        }
        // S3000-family firmware can report CHECK CONDITION after accepting a
        // MIDI-via-SCSI SEND. The proven transfer path treats 0 and 2 as
        // accepted; other transport/status values fail closed.
        if (status != 0 && status != 2) {
            sendError(ERR_MIDI_QUEUE_FULL, CMD_MIDI_SEND_BATCH,
                      "Akai rejected live MIDI event");
            return;
        }
        ++accepted;
        g.lastActivity = millis();
    }

    uint8_t receipt[4] = {};
    write16(receipt, accepted);
    write16(receipt + 2, 0); // synchronous route has no retained queue
    sendAck(receipt, sizeof(receipt));
}

__attribute__((noinline))
void handleMIDIFlush(const uint8_t *payload, uint16_t length) {
    if (!g.direct) {
        sendError(ERR_MIDI_UNAVAILABLE, CMD_MIDI_FLUSH,
                  "no active Akai live MIDI session");
    } else if (length != 1 || payload[0] != 0) {
        sendError(ERR_INVALID_LENGTH, CMD_MIDI_FLUSH,
                  "only Akai MIDI port zero is available");
    } else {
        // Dispatch is synchronous, so there is no firmware queue to discard.
        // The ACK is still useful as an ordering barrier before panic/close.
        g.lastActivity = millis();
        sendAck();
    }
}

void failBulkStream(const char *message) {
    g_bulk.failed = true;
    strncpy(g_bulk.failure, message ? message : "pipelined SCSI write failed",
            sizeof(g_bulk.failure) - 1);
    g_bulk.failure[sizeof(g_bulk.failure) - 1] = '\0';
}

void finishBulkStreamResponse() {
    if (!g_bulk.finishPending || g_bulk.commandRunning) return;
    if (g_bulk.failed) {
        sendError(ERR_SCSI_COMMAND, CMD_AKAI_BULK_FINISH, g_bulk.failure);
        resetBulkStream();
        return;
    }
    if (!g_bulk.commandComplete || g_bulk.count
        || g_bulk.acceptedBytes != g_bulk.expectedBytes
        || g_bulk.consumedBytes != g_bulk.expectedBytes
        || g_bulk.sentBytes != g_bulk.expectedBytes
        || g_bulk.nextSequence != g_bulk.expectedChunks) {
        sendError(ERR_AKAI_SESSION_STATE, CMD_AKAI_BULK_FINISH,
                  "pipelined byte/count mismatch");
        resetBulkStream();
        return;
    }
    uint8_t result[20] = {};
    write32(result, g_bulk.sentBytes);
    write32(result + 4, g_bulk.commandMicros);
    write32(result + 8, g_bulk.startupWaitMicros);
    write32(result + 12, g_bulk.starvationMicros);
    write32(result + 16, g_bulk.starvationEvents);
    sendAck(result, sizeof(result));
    resetBulkStream();
}

void handleBulkBegin(const uint8_t *payload, uint16_t length) {
    if (!g.direct) {
        sendError(ERR_AKAI_SESSION_STATE, CMD_AKAI_BULK_BEGIN,
                  "no active session");
        return;
    }
    if (g_bulk.active || g_bulk.commandRunning || g_bulkRead.active) {
        sendError(ERR_DIRECT_BUSY, CMD_AKAI_BULK_BEGIN,
                  "bulk stream already active");
        return;
    }
    if (length != 8 && length != 9) {
        sendError(ERR_INVALID_LENGTH, CMD_AKAI_BULK_BEGIN,
                  "want total bytes + chunk count + optional transport");
        return;
    }
    const uint32_t bytes = read32(payload);
    const uint32_t chunks = read32(payload + 4);
    const bool vendorTransport = length == 9 && payload[8] == 1;
    if (length == 9 && payload[8] > 1) {
        sendError(ERR_INVALID_LENGTH, CMD_AKAI_BULK_BEGIN,
                  "unknown bulk transport");
        return;
    }
    if (vendorTransport && !tud_vendor_mounted()) {
        sendError(ERR_DIRECT_BUSY, CMD_AKAI_BULK_BEGIN,
                  "native USB bulk interface is not claimed");
        return;
    }
    const uint32_t calculated = uint32_t(
        (uint64_t(bytes) + uint32_t(MAX_PIPELINED_PCM) - 1)
        / uint32_t(MAX_PIPELINED_PCM));
    // SEND(6) carries a 24-bit byte count. The Swift client splits unusually
    // large samples into multiple continuous windows before reaching here.
    if (bytes < 512 || bytes > 0x00FFFFFEu || (bytes & 1u)
        || !chunks || chunks != calculated) {
        sendError(ERR_INVALID_LENGTH, CMD_AKAI_BULK_BEGIN,
                  "invalid bulk byte/chunk count");
        return;
    }

    resetBulkStream();
    g_bulk.active = true;
    g_bulk.vendorTransport = vendorTransport;
    g_bulk.expectedBytes = bytes;
    g_bulk.expectedChunks = chunks;
    if (vendorTransport) {
        CoreMutex mutex(&USB.mutex, false);
        if (mutex) {
            tud_task();
            tud_vendor_read_flush();
        }
    }
    uint8_t negotiated[4] = {};
    write16(negotiated, MAX_PIPELINED_PCM);
    negotiated[2] = BULK_SLOT_COUNT;
    // Bit 0: one SCSI command spans every negotiated USB chunk.
    // Bit 1: PCM will arrive on the vendor-specific bulk OUT endpoint.
    negotiated[3] = uint8_t(1 | (vendorTransport ? 2 : 0));
    g_bulk.commandRunning = true;
    sendAck(negotiated, sizeof(negotiated));

    uint8_t cdb[6] = {
        0x0C, 0x00,
        uint8_t(bytes >> 16), uint8_t(bytes >> 8), uint8_t(bytes),
        0x80,
    };
    const uint32_t started = micros();
    const int status = scsiInitiatorRunCommand(
        g.directTarget, cdb, sizeof(cdb), nullptr, 0,
        g_bulk.slots[0].data, bytes, false, 30000, true, true);
    g_bulk.commandMicros = uint32_t(micros() - started);
    g_bulk.commandRunning = false;

    if (g_bulk.abortRequested) {
        resetBulkStream();
        return;
    }
    if (status != 0) {
        char detail[64];
        if (status == -4) {
            snprintf(detail, sizeof(detail),
                     "continuous SCSI DATA OUT timed out after %lu bytes",
                     (unsigned long)g_bulk.consumedBytes);
        } else {
            snprintf(detail, sizeof(detail),
                     "continuous SCSI DATA OUT status %d after %lu bytes",
                     status, (unsigned long)g_bulk.consumedBytes);
        }
        failBulkStream(detail);
    } else {
        g_bulk.sentBytes = bytes;
        g_bulk.commandComplete = true;
    }
    g.lastActivity = millis();
    finishBulkStreamResponse();
}

void handleBulkData(const uint8_t *payload, uint16_t length) {
    if (!g.direct || !g_bulk.active) {
        sendError(ERR_AKAI_SESSION_STATE, CMD_AKAI_BULK_DATA,
                  "no active bulk stream");
        return;
    }
    if (g_bulk.failed) {
        sendError(ERR_SCSI_COMMAND, CMD_AKAI_BULK_DATA, g_bulk.failure);
        return;
    }
    if (g_bulk.finishPending || g_bulk.abortRequested) {
        sendError(ERR_AKAI_SESSION_STATE, CMD_AKAI_BULK_DATA,
                  "bulk stream is closing");
        return;
    }
    if (g_bulk.vendorTransport) {
        sendError(ERR_AKAI_SESSION_STATE, CMD_AKAI_BULK_DATA,
                  "PCM is using the native USB bulk endpoint");
        return;
    }
    if (length <= 4 || length > 4 + MAX_PIPELINED_PCM
        || ((length - 4) & 1u)) {
        sendError(ERR_INVALID_LENGTH, CMD_AKAI_BULK_DATA,
                  "invalid pipelined PCM chunk");
        return;
    }
    const uint32_t sequence = read32(payload);
    const uint16_t byteCount = uint16_t(length - 4);
    if (sequence != g_bulk.nextSequence
        || uint64_t(g_bulk.acceptedBytes) + byteCount > g_bulk.expectedBytes) {
        sendError(ERR_AKAI_SESSION_STATE, CMD_AKAI_BULK_DATA,
                  "bulk sequence/length mismatch");
        return;
    }
    if (g_bulk.count >= BULK_SLOT_COUNT) {
        sendError(ERR_DIRECT_BUSY, CMD_AKAI_BULK_DATA,
                  "pipelined PCM queue is full");
        return;
    }

    BulkSlot &slot = g_bulk.slots[g_bulk.tail];
    slot.sequence = sequence;
    slot.acceptedBytes = g_bulk.acceptedBytes + byteCount;
    slot.length = byteCount;
    memcpy(slot.data, payload + 4, byteCount);
    slot.occupied = true;
    g_bulk.tail = uint8_t((g_bulk.tail + 1) % BULK_SLOT_COUNT);
    ++g_bulk.count;
    ++g_bulk.nextSequence;
    g_bulk.acceptedBytes += byteCount;
}

void handleBulkFinish(const uint8_t *payload, uint16_t length) {
    if (!g.direct || !g_bulk.active) {
        sendError(ERR_AKAI_SESSION_STATE, CMD_AKAI_BULK_FINISH,
                  "no active bulk stream");
        return;
    }
    if (length != 4 || read32(payload) != g_bulk.expectedChunks) {
        sendError(ERR_AKAI_SESSION_STATE, CMD_AKAI_BULK_FINISH,
                  "incomplete bulk stream");
        return;
    }
    // IOUSBHost completes a native OUT request when the RP2350 controller has
    // accepted its final packet. TinyUSB may not yet have promoted that last
    // controller window into the vendor FIFO/counters. Treat FINISH as an
    // ordered fence for the native lane and defer the exact byte/sequence
    // checks to finishBulkStreamResponse() after SCSI DATA OUT completes.
    // Framed CDC retains its immediate validation because every frame was
    // explicitly acknowledged before the Mac can send FINISH.
    if (!g_bulk.vendorTransport
        && (g_bulk.nextSequence != g_bulk.expectedChunks
            || g_bulk.acceptedBytes != g_bulk.expectedBytes)) {
        sendError(ERR_AKAI_SESSION_STATE, CMD_AKAI_BULK_FINISH,
                  "incomplete bulk stream");
        return;
    }
    g_bulk.finishPending = true;
    finishBulkStreamResponse();
}

void handleBulkAbort(uint16_t length) {
    if (length != 0) {
        sendError(ERR_INVALID_LENGTH, CMD_AKAI_BULK_ABORT,
                  "abort has no payload");
        return;
    }
    if (!g_bulk.active) {
        sendAck(); // Idempotent cleanup after a failed FINISH response.
        return;
    }
    g_bulk.abortRequested = true;
    sendAck();
    if (!g_bulk.commandRunning) resetBulkStream();
}

void failBulkReadStream(const char *message) {
    g_bulkRead.failed = true;
    strncpy(g_bulkRead.failure,
            message ? message : "continuous SCSI read failed",
            sizeof(g_bulkRead.failure) - 1);
    g_bulkRead.failure[sizeof(g_bulkRead.failure) - 1] = '\0';
}

void finishBulkReadStreamResponse() {
    if (!g_bulkRead.finishPending || g_bulkRead.commandRunning) return;
    if (g_bulkRead.failed) {
        sendError(ERR_SCSI_COMMAND, CMD_AKAI_BULK_READ_FINISH,
                  g_bulkRead.failure);
        resetBulkReadStream();
        return;
    }
    if (!g_bulkRead.commandComplete
        || g_bulkRead.sentBytes != g_bulkRead.expectedBytes) {
        sendError(ERR_AKAI_SESSION_STATE, CMD_AKAI_BULK_READ_FINISH,
                  "native DATA IN byte/count mismatch");
        resetBulkReadStream();
        return;
    }

    uint8_t result[24] = {};
    write32(result, g_bulkRead.sentBytes);
    write32(result + 4, g_bulkRead.commandMicros);
    write32(result + 8, g_bulkRead.startupWaitMicros);
    write32(result + 12, g_bulkRead.starvationMicros);
    write32(result + 16, g_bulkRead.starvationEvents);
    write32(result + 20, g_bulkRead.checksum);
    sendAck(result, sizeof(result));
    resetBulkReadStream();
}

void handleBulkReadBegin(const uint8_t *payload, uint16_t length) {
    if (!g.direct) {
        sendError(ERR_AKAI_SESSION_STATE, CMD_AKAI_BULK_READ_BEGIN,
                  "no active session");
        return;
    }
    if (g_bulk.active || g_bulkRead.active || g_bulkRead.commandRunning) {
        sendError(ERR_DIRECT_BUSY, CMD_AKAI_BULK_READ_BEGIN,
                  "bulk stream already active");
        return;
    }
    if (length != 4) {
        sendError(ERR_INVALID_LENGTH, CMD_AKAI_BULK_READ_BEGIN,
                  "want exact DATA IN byte count");
        return;
    }
    const uint32_t bytes = read32(payload);
    if (bytes < 512 || bytes > 0x00FFFFFEu || (bytes & 1u)) {
        sendError(ERR_INVALID_LENGTH, CMD_AKAI_BULK_READ_BEGIN,
                  "invalid DATA IN byte count");
        return;
    }
    if (!tud_vendor_mounted()) {
        sendError(ERR_DIRECT_BUSY, CMD_AKAI_BULK_READ_BEGIN,
                  "native USB bulk interface is not claimed");
        return;
    }

    {
        CoreMutex mutex(&USB.mutex, false);
        if (mutex) {
            tud_task();
            tud_vendor_write_flush();
        }
    }
    resetBulkReadStream();
    g_bulkRead.active = true;
    g_bulkRead.commandRunning = true;
    g_bulkRead.expectedBytes = bytes;
    g_bulkRead.lastProgressMillis = millis();

    uint8_t negotiated[4] = {};
    write16(negotiated, CFG_TUD_VENDOR_TX_BUFSIZE);
    negotiated[2] = g_vendorEndpointIn;
    // Bit 0: one SCSI RECEIVE spans the complete native USB IN payload.
    negotiated[3] = 1;
    sendAck(negotiated, sizeof(negotiated));

    const uint8_t cdb[6] = {
        0x0E, 0x00,
        uint8_t(bytes >> 16), uint8_t(bytes >> 8), uint8_t(bytes), 0x80,
    };
    const uint32_t started = micros();
    int status = scsiInitiatorRunCommand(
        g.directTarget, cdb, sizeof(cdb), g.body + 8, bytes,
        nullptr, 0, false, 300000, true, true);
    g_bulkRead.commandMicros = uint32_t(micros() - started);
    g_bulkRead.commandRunning = false;

    if (g_bulkRead.abortRequested) {
        resetBulkReadStream();
        return;
    }
    if (status != 0) {
        char detail[64];
        if (status == -4) {
            snprintf(detail, sizeof(detail),
                     "continuous SCSI DATA IN timed out after %lu bytes",
                     (unsigned long)g_bulkRead.sentBytes);
        } else {
            snprintf(detail, sizeof(detail),
                     "continuous SCSI DATA IN status %d after %lu bytes",
                     status, (unsigned long)g_bulkRead.sentBytes);
        }
        failBulkReadStream(detail);
    } else if (g_bulkRead.sentBytes != bytes) {
        failBulkReadStream("continuous SCSI DATA IN ended short");
    } else {
        g_bulkRead.commandComplete = true;
    }
    g.lastActivity = millis();
    finishBulkReadStreamResponse();
}

void handleBulkReadFinish(uint16_t length) {
    if (length != 0) {
        sendError(ERR_INVALID_LENGTH, CMD_AKAI_BULK_READ_FINISH,
                  "finish has no payload");
        return;
    }
    if (!g.direct || !g_bulkRead.active) {
        sendError(ERR_AKAI_SESSION_STATE, CMD_AKAI_BULK_READ_FINISH,
                  "no active DATA IN stream");
        return;
    }
    g_bulkRead.finishPending = true;
    finishBulkReadStreamResponse();
}

void handleBulkReadAbort(uint16_t length) {
    if (length != 0) {
        sendError(ERR_INVALID_LENGTH, CMD_AKAI_BULK_READ_ABORT,
                  "abort has no payload");
        return;
    }
    if (!g_bulkRead.active) {
        sendAck();
        return;
    }
    g_bulkRead.abortRequested = true;
    sendAck();
    if (!g_bulkRead.commandRunning) resetBulkReadStream();
}

void handleUSBBulkBenchmarkBegin(const uint8_t *payload, uint16_t length) {
    if (length != 4) {
        sendError(ERR_INVALID_LENGTH, CMD_USB_BULK_BENCH_BEGIN,
                  "want benchmark byte count");
        return;
    }
    const uint32_t bytes = read32(payload);
    if (bytes < 64 || bytes > 64u * 1024u * 1024u) {
        sendError(ERR_INVALID_LENGTH, CMD_USB_BULK_BENCH_BEGIN,
                  "invalid benchmark byte count");
        return;
    }

    {
        CoreMutex mutex(&USB.mutex, false);
        if (mutex) {
            tud_task();
            tud_vendor_read_flush();
        }
    }
    g_usbBenchmark = USBBulkBenchmarkState{};
    g_usbBenchmark.active = true;
    g_usbBenchmark.expectedBytes = bytes;
    g_usbBenchmark.startedMicros = micros();

    uint8_t result[4] = {};
    write16(result, CFG_TUD_VENDOR_EPSIZE);
    write16(result + 2, CFG_TUD_VENDOR_RX_BUFSIZE);
    sendAck(result, sizeof(result));
}

void handleUSBBulkBenchmarkStatus(uint16_t length) {
    if (length != 0) {
        sendError(ERR_INVALID_LENGTH, CMD_USB_BULK_BENCH_STATUS,
                  "status has no payload");
        return;
    }
    pumpVendorBenchmark();
    uint8_t result[17] = {};
    result[0] = g_usbBenchmark.complete ? 1 : 0;
    write32(result + 1, g_usbBenchmark.expectedBytes);
    write32(result + 5, g_usbBenchmark.receivedBytes);
    write32(result + 9, g_usbBenchmark.elapsedMicros);
    write32(result + 13, g_usbBenchmark.checksum);
    sendAck(result, sizeof(result));
}

void dispatch(uint8_t opcode, const uint8_t *payload, uint16_t length) {
    g.lastActivity = millis();
    switch (opcode) {
    case CMD_PING: sendFrame(RSP_PONG); break;
    case CMD_GET_INFO: handleGetInfo(); break;
    case CMD_SUBSCRIBE: sendFrame(RSP_PONG); break;
    case CMD_LIST_FILES: handleList(payload, length); break;
    case CMD_FILE_OPEN_WRITE: handleOpenWrite(payload, length); break;
    case CMD_FILE_WRITE_CHUNK: handleWrite(payload, length); break;
    case CMD_FILE_CLOSE_COMMIT: handleCommit(); break;
    case CMD_FILE_READ_OPEN: handleOpenRead(payload, length); break;
    case CMD_FILE_READ_CHUNK: handleRead(payload, length); break;
    case CMD_QUIESCE:
        if (length != 1 || g.direct) sendError(g.direct ? ERR_DIRECT_BUSY : ERR_INVALID_LENGTH, opcode, "invalid quiesce");
        else { g.quiesce = payload[0] != 0; sendAck(); }
        break;
    case CMD_EJECT_CYCLE:
        if (length != 1 || payload[0] >= S2S_MAX_TARGETS) sendError(ERR_IMAGE_INDEX, opcode, "bad scsi id");
        else {
            image_config_t &img = scsiDiskGetImageConfig(payload[0]);
            if (!img.file.isOpen()) sendError(ERR_NO_IMAGE, opcode, "target not enabled");
            else { img.ejected = true; img.reinsert_after_eject = true; sendAck(); }
        }
        break;
    case CMD_REBOOT_BOOTLOADER:
        if (length != 0 || g.direct) {
            sendError(g.direct ? ERR_DIRECT_BUSY : ERR_INVALID_LENGTH, opcode,
                      "bootloader restart unavailable");
        } else {
            closeTransferFile(); g.quiesce = true; sendAck(); delay(100);
            platform_enter_bootloader();
        }
        break;
    case CMD_MIDI_SEND_BATCH: handleMIDISendBatch(payload, length); break;
    case CMD_MIDI_FLUSH: handleMIDIFlush(payload, length); break;
    case CMD_AKAI_DISCOVER: handleDiscovery(payload, length); break;
    case CMD_AKAI_SESSION_BEGIN: handleSessionBegin(payload, length); break;
    case CMD_AKAI_SCSI_EXECUTE: handleSCSIExecute(payload, length); break;
    case CMD_AKAI_BULK_BEGIN: handleBulkBegin(payload, length); break;
    case CMD_AKAI_BULK_DATA: handleBulkData(payload, length); break;
    case CMD_AKAI_BULK_FINISH: handleBulkFinish(payload, length); break;
    case CMD_AKAI_BULK_ABORT: handleBulkAbort(length); break;
    case CMD_AKAI_BULK_READ_BEGIN: handleBulkReadBegin(payload, length); break;
    case CMD_AKAI_BULK_READ_FINISH: handleBulkReadFinish(length); break;
    case CMD_AKAI_BULK_READ_ABORT: handleBulkReadAbort(length); break;
    case CMD_USB_BULK_BENCH_BEGIN: handleUSBBulkBenchmarkBegin(payload, length); break;
    case CMD_USB_BULK_BENCH_STATUS: handleUSBBulkBenchmarkStatus(length); break;
    case CMD_AKAI_SESSION_END:
        if (!g.direct) sendError(ERR_AKAI_SESSION_STATE, opcode, "no active session");
        else if (g_bulk.active || g_bulkRead.active) sendError(ERR_DIRECT_BUSY, opcode, "bulk stream active");
        else { restoreTargetMode(); sendAck(); }
        break;
    case CMD_EXIT: restoreTargetMode(); closeTransferFile(); g.quiesce = false; sendAck(); g.binary = false; break;
    default: sendError(ERR_INVALID_COMMAND, opcode, "unsupported command"); break;
    }
}

void consume(uint8_t byte) {
    switch (g.rx) {
    case RX_SYNC0: if (byte == SYNC0) g.rx = RX_SYNC1; break;
    case RX_SYNC1: g.rx = byte == SYNC1 ? RX_LEN0 : (byte == SYNC0 ? RX_SYNC1 : RX_SYNC0); break;
    case RX_LEN0: g.bodyLength = byte; g.rx = RX_LEN1; break;
    case RX_LEN1:
        g.bodyLength |= uint16_t(byte) << 8; g.bodyUsed = 0;
        if (!g.bodyLength || g.bodyLength > MAX_PAYLOAD + 1) resetParser(); else g.rx = RX_BODY;
        break;
    case RX_BODY: g.body[g.bodyUsed++] = byte; if (g.bodyUsed == g.bodyLength) g.rx = RX_CRC0; break;
    case RX_CRC0: g.wireCRC = byte; g.rx = RX_CRC1; break;
    case RX_CRC1:
        g.wireCRC |= uint16_t(byte) << 8;
        {
            const uint8_t opcode = g.body[0];
            const uint16_t payloadLength = g.bodyLength - 1;
            // USB bulk packets already provide hardware CRC plus retry. PCM
            // is the only high-volume payload and remains length/sequence
            // checked, so avoid hashing it a second time on the RP2350.
            // Commands and all non-PCM payloads retain the protocol CRC.
            const bool valid = g.body[0] == CMD_AKAI_BULK_DATA
                || crc16(g.body, g.bodyLength) == g.wireCRC;
            // Reset before dispatch. A PIO/DMA-backed handler can service USB
            // recursively while SCSI is active; the next frame must start in
            // a clean parser state instead of overwriting a completed frame.
            resetParser();
            if (!valid) sendError(ERR_CRC, opcode, "CRC mismatch");
            else dispatch(opcode, g.body + 1, payloadLength);
        }
        break;
    }
}

void pumpCDCInput() {
    // TinyUSB exposes a 256-byte CDC receive FIFO. Drain every available FIFO
    // window in one bounded call; this helper is also called while PIO/DMA is
    // moving PCM to the Akai so USB reception can overlap SCSI DATA OUT.
    uint8_t input[256];
    while (true) {
        size_t received = 0;
        {
            CoreMutex mutex(&USB.mutex, false);
            if (!mutex) break;
            tud_task();
            uint32_t available = tud_cdc_available();
            if (!available) break;
            uint32_t requested = available;
            if (requested > sizeof(input)) requested = sizeof(input);
            received = tud_cdc_read(input, requested);
        }
        if (!received) break;
        g.lastActivity = millis();
        for (size_t index = 0; index < received; ++index) consume(input[index]);
    }
}

// A credit ACK can be emitted while the parser already holds the beginning of
// the next back-to-back USB frame. Generic sendAck() stages through `g.body`,
// so using it here would overwrite that partial frame and manufacture a CRC
// failure. Keep this tiny response entirely on the stack.
void sendBulkCreditAck(uint32_t sequence, uint32_t acceptedBytes) {
    uint8_t payload[9] = {};
    payload[0] = 0;
    write32(payload + 1, sequence);
    write32(payload + 5, acceptedBytes);
    sendFrame(RSP_ACK, payload, sizeof(payload));
}
} // namespace

bool zbridge_binary_active() { return g.binary; }
bool zbridge_target_quiesced() { return g.quiesce || g.direct; }
void zbridge_stream_pump_usb() { pumpCDCInput(); }
bool zbridge_streaming_write_active() {
    return g_bulk.active && g_bulk.commandRunning;
}

uint32_t zbridge_stream_read(uint8_t *destination, uint32_t capacity) {
    if (!destination || !capacity || !zbridge_streaming_write_active()) return 0;

    if (g_bulk.vendorTransport) {
        uint32_t copied = 0;
        bool waiting = false;
        uint32_t waitStarted = 0;
        const bool startup = !g_bulk.consumedFirstByte;

        while (copied < capacity
               && g_bulk.consumedBytes < g_bulk.expectedBytes) {
            if (g_bulk.failed || g_bulk.abortRequested) break;

            uint32_t received = 0;
            {
                CoreMutex mutex(&USB.mutex, false);
                if (mutex) {
                    tud_task();
                    uint32_t requested = tud_vendor_available();
                    const uint32_t outputRoom = capacity - copied;
                    const uint32_t streamRemaining = g_bulk.expectedBytes
                        - g_bulk.consumedBytes;
                    if (requested > outputRoom) requested = outputRoom;
                    if (requested > streamRemaining) requested = streamRemaining;
                    if (requested) {
                        received = tud_vendor_read(destination + copied,
                                                   requested);
                    }
                }
            }

            if (received) {
                copied += received;
                g_bulk.acceptedBytes += received;
                g_bulk.consumedBytes += received;
                g_bulk.consumedFirstByte = true;
                // FINISH arrives over the independent CDC endpoint and can be
                // parsed by pumpCDCInput() immediately after the final native
                // bulk read. Publish logical chunk completion before pumping
                // CDC so FINISH never sees exact byte counts with a stale
                // sequence and rejects an otherwise complete transfer.
                if (g_bulk.consumedBytes == g_bulk.expectedBytes) {
                    g_bulk.nextSequence = g_bulk.expectedChunks;
                }
                g.lastActivity = millis();
                // CDC has its own endpoints. Service priority control/MIDI
                // between native-bulk reads even when PCM is arriving without
                // gaps, so a large sample never monopolizes USB.
                pumpCDCInput();
                continue;
            }

            if (!waiting) {
                waiting = true;
                waitStarted = micros();
            }
            pumpCDCInput();
            platform_reset_watchdog();
            if (uint32_t(millis() - g.lastActivity) > DIRECT_IDLE_MS) {
                failBulkStream("native USB PCM source timed out");
                break;
            }
            tight_loop_contents();
        }

        if (waiting) {
            const uint32_t waited = uint32_t(micros() - waitStarted);
            if (startup) g_bulk.startupWaitMicros += waited;
            else {
                g_bulk.starvationMicros += waited;
                ++g_bulk.starvationEvents;
            }
        }
        if (g_bulk.consumedBytes == g_bulk.expectedBytes) {
            // The vendor pipe is a continuous byte stream rather than framed
            // CDC chunks; retain the negotiated logical count so FINISH uses
            // the same integrity contract on both transports.
            g_bulk.nextSequence = g_bulk.expectedChunks;
        }
        return copied;
    }

    uint32_t copied = 0;
    while (copied < capacity) {
        if (g_bulk.failed || g_bulk.abortRequested) return copied;

        if (!g_bulk.count) {
            if (g_bulk.acceptedBytes >= g_bulk.expectedBytes) {
                failBulkStream("continuous PCM ring underrun");
                return copied;
            }

            const bool startup = !g_bulk.consumedFirstByte;
            const uint32_t waitStarted = micros();
            while (!g_bulk.count && !g_bulk.failed && !g_bulk.abortRequested) {
                pumpCDCInput();
                platform_reset_watchdog();
                if (uint32_t(millis() - g.lastActivity) > DIRECT_IDLE_MS) {
                    failBulkStream("USB PCM source timed out");
                    break;
                }
                tight_loop_contents();
            }
            const uint32_t waited = uint32_t(micros() - waitStarted);
            if (startup) {
                g_bulk.startupWaitMicros += waited;
            } else if (waited) {
                g_bulk.starvationMicros += waited;
                ++g_bulk.starvationEvents;
            }
            if (!g_bulk.count) return copied;
        }

        BulkSlot &slot = g_bulk.slots[g_bulk.head];
        if (!slot.occupied || g_bulk.headOffset >= slot.length) {
            failBulkStream("continuous PCM ring state mismatch");
            return copied;
        }
        const uint32_t available = uint32_t(slot.length - g_bulk.headOffset);
        const uint32_t wanted = capacity - copied;
        const uint32_t amount = available < wanted ? available : wanted;
        memcpy(destination + copied, slot.data + g_bulk.headOffset, amount);
        copied += amount;
        g_bulk.headOffset = uint16_t(g_bulk.headOffset + amount);
        g_bulk.consumedBytes += amount;
        g_bulk.consumedFirstByte = true;

        if (g_bulk.headOffset == slot.length) {
            const uint32_t completedSequence = slot.sequence;
            const uint32_t completedBytes = slot.acceptedBytes;
            slot.occupied = false;
            slot.length = 0;
            g_bulk.headOffset = 0;
            g_bulk.head = uint8_t((g_bulk.head + 1) % BULK_SLOT_COUNT);
            --g_bulk.count;

            // Protocol v7 ACKs are ring credits, not merely receipt notices.
            // Once this slot has been copied into the PIO feeder, the Mac may
            // safely replace it while the next slot is still reaching Akai.
            sendBulkCreditAck(completedSequence, completedBytes);
        }
    }
    return copied;
}

bool zbridge_streaming_read_active() {
    return g_bulkRead.active && g_bulkRead.commandRunning;
}

extern "C" bool zbridge_native_in_stream_active() {
    return g_bulkRead.active;
}

bool zbridge_stream_write(const uint8_t *source, uint32_t count) {
    if (!source || !count || !zbridge_streaming_read_active()) return false;

    uint32_t offset = 0;
    bool waiting = false;
    bool startupWait = !g_bulkRead.sentFirstByte;
    uint32_t waitStarted = 0;
    while (offset < count) {
        if (g_bulkRead.failed || g_bulkRead.abortRequested) return false;
        if (!tud_vendor_mounted()) {
            failBulkReadStream("native USB DATA IN endpoint disconnected");
            return false;
        }

        uint32_t written = 0;
        {
            CoreMutex mutex(&USB.mutex, false);
            if (mutex) {
                tud_task();
                uint32_t requested = tud_vendor_write_available();
                const uint32_t remaining = count - offset;
                if (requested > remaining) requested = remaining;
                if (requested) {
                    written = tud_vendor_write(source + offset, requested);
                    // Full windows are started and chained by TinyUSB itself.
                    // An explicit flush on every producer write races the IN
                    // completion callback's FIFO drain and can duplicate or
                    // skip a window. Only the final short FIFO needs forcing.
                    if (written
                        && g_bulkRead.sentBytes + written
                            == g_bulkRead.expectedBytes) {
                        tud_vendor_write_flush();
                    }
                }
            }
        }

        if (written) {
            if (waiting) {
                const uint32_t waited = uint32_t(micros() - waitStarted);
                if (startupWait) g_bulkRead.startupWaitMicros += waited;
                else {
                    g_bulkRead.starvationMicros += waited;
                    ++g_bulkRead.starvationEvents;
                }
                waiting = false;
                startupWait = false;
            }
            for (uint32_t index = 0; index < written; ++index) {
                g_bulkRead.checksum ^= source[offset + index];
                g_bulkRead.checksum *= 16777619u;
            }
            offset += written;
            g_bulkRead.sentBytes += written;
            g_bulkRead.sentFirstByte = true;
            g_bulkRead.lastProgressMillis = millis();
            g.lastActivity = g_bulkRead.lastProgressMillis;
            // CDC owns separate endpoints. This is the bounded opening used by
            // abort/recovery now and by priority live MIDI scheduling later.
            pumpCDCInput();
            continue;
        }

        if (!waiting) {
            waiting = true;
            waitStarted = micros();
        }
        pumpCDCInput();
        platform_reset_watchdog();
        if (uint32_t(millis() - g_bulkRead.lastProgressMillis)
            > DIRECT_IDLE_MS) {
            failBulkReadStream("native USB DATA IN consumer timed out");
            return false;
        }
        tight_loop_contents();
    }
    return true;
}

bool zbridge_poll() {
    uint32_t now = millis();
    pumpVendorBenchmark();
    if (g.direct && uint32_t(now - g.lastActivity) > DIRECT_IDLE_MS) restoreTargetMode();
    if (g.binary && !g.direct && uint32_t(now - g.lastActivity) > BINARY_IDLE_MS) {
        closeTransferFile(); g.quiesce = false; g.binary = false; resetParser();
    }
    if (!Serial || Serial.available() <= 0) return g.binary;
    if (!g.binary && Serial.peek() != SYNC0) return false;
    g.binary = true; g.lastActivity = now;
    pumpCDCInput();
    return true;
}

void zbridge_usb_init() {
    if (g_vendorInterfaceID) return;

    USB.disconnect();
    g_vendorEndpointIn = USB.registerEndpointIn();
    g_vendorEndpointOut = USB.registerEndpointOut();
    g_vendorStringID = USB.registerString("ZBridge Bulk");
    g_vendorInterfaceID = USB.registerInterface(
        1, vendorInterfaceDescriptor, nullptr, TUD_VENDOR_DESC_LEN, 2, 0);
    USB.connect();
}
