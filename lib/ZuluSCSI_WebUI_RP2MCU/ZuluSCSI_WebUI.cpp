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

// ZuluSCSI WebUI I2C server - state machine and message handlers.
// Mirrors the media management capabilities of the USB serial console.
//
// zuluscsi.ini [WebUI] section keys:
//   WiFiSSID       = "my_wifi_ssid"
//   WiFiPassword   = "my_wifi_password"
//   StaticIP   = "192.168.1.100"
//   Netmask    = "255.255.255.0"
//   Gateway    = "192.168.1.1"
#ifdef ZULUCONTROL_FIRMWARE

#include "ZuluSCSI_WebUI.h"
#include "ZuluSCSI_WebUI_I2CServer.h"
#include <ZuluSCSI_log.h>
#include <ZuluSCSI_control_api.h>
#include <minIni.h>
#include <Arduino.h>
#include <string.h>
#include <stdio.h>

#define CONFIGFILE "zuluscsi.ini"

// ── State machine ─────────────────────────────────────────────────────────────

enum class WebUIState
{
    Disabled,       // No SSID configured - WebUI off
    Searching,      // Polling for WebUI board presence
    SendReset,      // Sending RESET to flush WebUI board state
    SendAPIVer,     // Sending server API version
    WaitAPIVer,     // Waiting for WebUI board to reply with its API version
    SendSSID,       // Sending WiFi SSID
    SendPassword,   // Sending WiFi password
    SendStaticIP,   // Sending optional static IP info
    SendConnect,    // Sending WIFI_CONNECT
    WaitIP,         // Waiting for WebUI board to send its IP address
    Running,        // Normal operation
};

static WebUIState  g_state       = WebUIState::Disabled;
static bool        g_enabled     = false;
static bool        g_subscribed  = false;  // I2C WebUI client subscribed to status updates
static bool        g_sd_ready    = false;  // SD card currently present and ready

static char        g_ssid[64]    = {0};
static char        g_pass[64]    = {0};
static char        g_static_ip[20] = {0};
static char        g_netmask[20]   = {0};
static char        g_gateway[20]   = {0};
static bool        g_has_static_ip = false;

static uint32_t    g_last_poll_ms    = 0;
static uint32_t    g_last_status_ms  = 0;
static uint32_t    g_wait_start_ms   = 0;

#define POLL_INTERVAL_MS   150
#define STATUS_INTERVAL_MS 3000
#define WAIT_TIMEOUT_MS    5000

// ── Helpers ───────────────────────────────────────────────────────────────────

static uint32_t ms_now()
{
    return (uint32_t)millis();
}

static bool elapsed(uint32_t start, uint32_t period)
{
    return (uint32_t)(ms_now() - start) >= period;
}

// Write a compact JSON object for one image into buf (max buflen bytes).
// Returns number of bytes written (not including null terminator).
static int image_json(char *buf, size_t buflen, int idx, const char *filename,
                      const char *full_path, uint64_t size, bool is_dir)
{
    return snprintf(buf, buflen,
        "{\"idx\":%d,\"filename\":\"%s\",\"path\":\"%s\",\"size\":%llu,\"isDir\":%s}",
        idx, filename, full_path, (unsigned long long)size, is_dir ? "true" : "false");
}

// Build full system-status JSON and send it to the WebUI board.
static void send_status_json()
{
    uint8_t ids[S2S_MAX_TARGETS];
    int dev_count = controlListRemovableDevices(ids, S2S_MAX_TARGETS);

    char json[480] = {0};
    int pos = 0;
    pos += snprintf(json + pos, sizeof(json) - pos, "{\"devices\":[");

    for (int d = 0; d < dev_count && pos < (int)sizeof(json) - 80; d++)
    {
        uint8_t id = ids[d];
        char imgpath[128] = {0};
        bool is_ejected = true;
        bool has_image = controlGetMediaStatus(id, imgpath, sizeof(imgpath), &is_ejected);
        const char *type = controlGetDeviceTypeName(id);

        // Truncate very long image paths to keep JSON within Wire buffer limits
        char shortname[48] = {0};
        const char *slash = strrchr(imgpath, '/');
        strncpy(shortname, slash ? slash + 1 : imgpath, sizeof(shortname) - 1);

        if (d > 0) json[pos++] = ',';
        pos += snprintf(json + pos, sizeof(json) - pos,
            "{\"id\":%d,\"type\":\"%s\",\"image\":\"%s\",\"ejected\":%s}",
            id, type ? type : "Unknown",
            has_image ? shortname : "",
            is_ejected ? "true" : "false");
    }
    pos += snprintf(json + pos, sizeof(json) - pos, "]}");

    webuiI2CSend(I2C_SERVER_SYSTEM_STATUS_JSON, json);
}

// Send JSON array of removable devices to WebUI board.
static void send_device_list_json()
{
    uint8_t ids[S2S_MAX_TARGETS];
    int count = controlListRemovableDevices(ids, S2S_MAX_TARGETS);

    char json[256] = {0};
    int pos = 0;
    pos += snprintf(json + pos, sizeof(json) - pos, "[");

    for (int i = 0; i < count; i++)
    {
        const char *type = controlGetDeviceTypeName(ids[i]);
        if (i > 0) json[pos++] = ',';
        pos += snprintf(json + pos, sizeof(json) - pos,
            "{\"id\":%d,\"type\":\"%s\"}", ids[i], type ? type : "Unknown");
    }
    snprintf(json + pos, sizeof(json) - pos, "]");

    webuiI2CSend(I2C_SERVER_DEVICE_LIST_JSON, json);
}

// ── Image listing helpers ─────────────────────────────────────────────────────

struct FilenameListCtx
{
    uint8_t scsi_id;
    bool    started;
    int     count;
};

static void filename_list_cb(int idx, const char *filename, const char *full_path,
                              uint64_t size, bool is_dir, void *userdata)
{
    FilenameListCtx *ctx = (FilenameListCtx *)userdata;

    if (!ctx->started)
    {
        // Signal start of filename cache update
        uint8_t scsi_hdr[1] = {ctx->scsi_id};
        webuiI2CSend(I2C_SERVER_UPDATE_FILENAME_CACHE, scsi_hdr, 1);
        ctx->started = true;
    }

    // payload: [scsi_id][full_path...] - full_path required by controlLoadImage
    char payload[256];
    payload[0] = (char)ctx->scsi_id;
    strncpy(payload + 1, full_path, sizeof(payload) - 2);
    payload[sizeof(payload) - 1] = '\0';
    uint16_t pathlen = (uint16_t)strnlen(full_path, sizeof(payload) - 2);
    webuiI2CSend(I2C_SERVER_IMAGE_FILENAME, (const uint8_t *)payload,
                 1 + pathlen);
    ctx->count++;
}

struct ImageJsonCtx
{
    uint8_t scsi_id;
    bool    started;
};

static void image_json_cb(int idx, const char *filename, const char *full_path,
                           uint64_t size, bool is_dir, void *userdata)
{
    ImageJsonCtx *ctx = (ImageJsonCtx *)userdata;

    char jbuf[200];
    int jlen = image_json(jbuf, sizeof(jbuf) - 1, idx, filename, full_path, size, is_dir);
    if (jlen <= 0) return;

    // payload: [scsi_id][json...]
    char payload[210];
    payload[0] = (char)ctx->scsi_id;
    memcpy(payload + 1, jbuf, (size_t)jlen);
    webuiI2CSend(I2C_SERVER_IMAGE_JSON, (const uint8_t *)payload, 1 + (uint16_t)jlen);
}

static void send_filenames_for_device(uint8_t scsi_id)
{
    // Send cache-reset header unconditionally so the I2C WebUI client clears its cache
    // even when there are zero images (e.g. after SD card removal).
    uint8_t scsi_hdr[1] = {scsi_id};
    webuiI2CSend(I2C_SERVER_UPDATE_FILENAME_CACHE, scsi_hdr, 1);

    // started=true: reset already sent above, callback must not send it again
    FilenameListCtx ctx = {scsi_id, true, 0};
    controlListImages(scsi_id, filename_list_cb, &ctx);

    // End-of-list sentinel: [scsi_id] with no path bytes
    uint8_t end_hdr[1] = {scsi_id};
    webuiI2CSend(I2C_SERVER_IMAGE_FILENAME, end_hdr, 1);
    dbgmsg("WebUI: sent ", ctx.count, " filenames for SCSI ID ", (int)scsi_id);
}

static void send_all_filenames()
{
    uint8_t ids[S2S_MAX_TARGETS];
    int count = controlListRemovableDevices(ids, 8);
    for (int i = 0; i < count; i++)
    {
        send_filenames_for_device(ids[i]);
    }
}

// ── Message dispatcher (Running state) ───────────────────────────────────────

static void handle_client_message(uint8_t cmd, const uint8_t *payload, uint16_t length)
{
    switch (cmd)
    {
        case I2C_CLIENT_NOOP:
            break;

        case I2C_CLIENT_API_VERSION:
            // WebUI board has reset - restart handshake
            dbgmsg("WebUI: board sent API version (restart), resetting");
            g_state = WebUIState::SendReset;
            g_subscribed = false;
            break;

        case I2C_CLIENT_SUBSCRIBE_STATUS_JSON:
            dbgmsg("WebUI: subscribed to status updates");
            g_subscribed = true;
            send_status_json();
            {
                uint8_t sd_payload[1] = {g_sd_ready ? I2C_SERVER_SD_PRESENT : I2C_SERVER_SD_NOT_PRESENT};
                webuiI2CSend(I2C_SERVER_SD_STATUS_CHANGE, sd_payload, 1);
            }
            send_all_filenames();
            g_last_status_ms = ms_now();
            break;

        case I2C_CLIENT_FETCH_DEVICE_LIST:
            dbgmsg("WebUI: sending device list");
            send_device_list_json();
            break;

        case I2C_CLIENT_FETCH_FILENAMES:
        {
            uint8_t scsi_id = (length > 0) ? payload[0] : 0;
            dbgmsg("WebUI: listing filenames for SCSI ID ", (int)scsi_id);
            send_filenames_for_device(scsi_id);
            break;
        }

        case I2C_CLIENT_FETCH_IMAGES_JSON:
        {
            uint8_t scsi_id = (length > 0) ? payload[0] : 0;
            logmsg("WebUI: listing images JSON for SCSI ID ", (int)scsi_id);
            ImageJsonCtx ctx = {scsi_id, false};
            controlListImages(scsi_id, image_json_cb, &ctx);
            // Empty I2C_SERVER_IMAGE_JSON signals end-of-list
            uint8_t end_hdr[1] = {scsi_id};
            webuiI2CSend(I2C_SERVER_IMAGE_JSON, end_hdr, 1);
            break;
        }

        case I2C_CLIENT_FETCH_ITR_IMAGE:
        {
            // Iterator mode: send one image per request.
            // Reuses image_json_cb but the I2C WebUI client handles iterator state.
            // For simplicity, send all images and mark done with empty payload.
            uint8_t scsi_id = (length > 0) ? payload[0] : 0;
            ImageJsonCtx ctx = {scsi_id, false};
            controlListImages(scsi_id, image_json_cb, &ctx);
            uint8_t end_hdr[1] = {scsi_id};
            webuiI2CSend(I2C_SERVER_IMAGE_JSON, end_hdr, 1);
            break;
        }

        case I2C_CLIENT_LOAD_IMAGE:
        {
            if (length < 2)
            {
                logmsg("WebUI: LOAD_IMAGE missing SCSI ID or path");
                break;
            }
            uint8_t scsi_id = payload[0];
            const char *path = (const char *)(payload + 1);
            dbgmsg("WebUI: loading image \"", path, "\" on SCSI ID ", (int)scsi_id);
            if (!controlLoadImage(scsi_id, path))
            {
                logmsg("WebUI: controlLoadImage failed for SCSI ID ", (int)scsi_id);
            }
            if (g_subscribed) send_status_json();
            break;
        }

        case I2C_CLIENT_EJECT_IMAGE:
        {
            uint8_t scsi_id = (length > 0) ? payload[0] : 0;
            dbgmsg("WebUI: ejecting media on SCSI ID ", (int)scsi_id);
            if (!controlEjectMedia(scsi_id))
            {
                logmsg("WebUI: controlEjectMedia failed for SCSI ID ", (int)scsi_id);
            }
            if (g_subscribed) send_status_json();
            break;
        }

        case I2C_CLIENT_INSERT_MEDIA:
        {
            uint8_t scsi_id = (length > 0) ? payload[0] : 0;
            logmsg("WebUI: inserting media on SCSI ID ", (int)scsi_id);
            if (!controlInsertMedia(scsi_id))
            {
                logmsg("WebUI: controlInsertMedia failed for SCSI ID ", (int)scsi_id);
            }
            if (g_subscribed) send_status_json();
            break;
        }

        case I2C_CLIENT_FETCH_SSID:
            webuiI2CSend(I2C_SERVER_SSID, g_ssid);
            break;

        case I2C_CLIENT_FETCH_SSID_PASS:
            webuiI2CSend(I2C_SERVER_SSID_PASS, g_pass);
            break;

        case I2C_CLIENT_IP_ADDRESS:
        {
            char ipbuf[32] = {0};
            if (length > 0 && length < 32)
            {
                memcpy(ipbuf, payload, length);
                ipbuf[length] = '\0';
            }
            logmsg("WebUI: IP address: ", ipbuf);
            webuiI2CSend(I2C_SERVER_IP_ADDRESS_ACK);
            break;
        }

        case I2C_CLIENT_LOG_MSG:
        {
            if (length > 1)
            {
                char prefix = (char)payload[0];
                const char *msg = (const char *)(payload + 1);
                if (prefix == 'd')
                    dbgmsg("WebUI board: ", msg);
                else
                    logmsg("WebUI board: ", msg);
            }
            break;
        }

        default:
            logmsg("WebUI: unhandled client command 0x", (int)cmd);
            break;
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

void zuluWebUIInit()
{
    ini_gets("WebUI", "WiFiSSID",     "", g_ssid,      sizeof(g_ssid),      CONFIGFILE);
    ini_gets("WebUI", "WiFiPassword", "", g_pass,      sizeof(g_pass),      CONFIGFILE);
    ini_gets("WebUI", "StaticIP", "", g_static_ip, sizeof(g_static_ip), CONFIGFILE);
    ini_gets("WebUI", "Netmask",  "", g_netmask,   sizeof(g_netmask),   CONFIGFILE);
    ini_gets("WebUI", "Gateway",  "", g_gateway,   sizeof(g_gateway),   CONFIGFILE);

    g_has_static_ip = (g_static_ip[0] != '\0');

    if (g_ssid[0] == '\0')
    {
        logmsg("WebUI: No SSID configured in [WebUI] section of ", CONFIGFILE, " - disabled");
        g_state   = WebUIState::Disabled;
        g_enabled = false;
        return;
    }

    logmsg("WebUI: configured for SSID \"", g_ssid, "\"");
    g_state   = WebUIState::Searching;
    g_enabled = true;
    g_last_poll_ms = ms_now();
}

bool zuluWebUIEnabled()
{
    return g_enabled && g_state == WebUIState::Running;
}

void zuluWebUINotifySDCardReady()
{
    g_sd_ready = true;
    // Send the SD status change whenever the board is reachable, regardless of
    // WebUI state machine phase.  zuluWebUIInit() resets g_state to Searching
    // on SD insertion which would otherwise silently drop the push.
    if (webuiI2CWebUIPresent())
    {
        uint8_t sd_payload[1] = {I2C_SERVER_SD_PRESENT};
        webuiI2CSend(I2C_SERVER_SD_STATUS_CHANGE, sd_payload, 1);
    }
    if (g_subscribed && g_state == WebUIState::Running)
    {
        dbgmsg("WebUI: SD card ready, pushing filename cache");
        send_all_filenames();
    }
}

void zuluWebUINotifySDCardRemoved()
{
    g_sd_ready = false;
    if (webuiI2CWebUIPresent())
    {
        uint8_t sd_payload[1] = {I2C_SERVER_SD_NOT_PRESENT};
        webuiI2CSend(I2C_SERVER_SD_STATUS_CHANGE, sd_payload, 1);
    }
    if (g_subscribed && g_state == WebUIState::Running)
    {
        dbgmsg("WebUI: SD card removed, clearing filename cache");
        send_all_filenames();
    }
}

void zuluWebUITask()
{
    if (!g_enabled) return;
    if (!elapsed(g_last_poll_ms, POLL_INTERVAL_MS)) return;
    g_last_poll_ms = ms_now();

    static uint8_t rx_payload[WEBUI_MAX_MSG_SIZE + 1];
    uint8_t  rx_cmd    = I2C_CLIENT_NOOP;
    uint16_t rx_length = 0;

    switch (g_state)
    {
        case WebUIState::Searching:
        {
            // Probe for WebUI board by attempting a read
            bool got = webuiI2CReceive(&rx_cmd, rx_payload, &rx_length);
            if (got || webuiI2CWebUIPresent())
            {
                dbgmsg("WebUI: board detected, starting handshake");
                g_state = WebUIState::SendReset;
            }
            break;
        }

        case WebUIState::SendReset:
            if (webuiI2CSend(I2C_SERVER_RESET))
            {
                dbgmsg("WebUI: sent RESET to board");
                g_state = WebUIState::SendAPIVer;
            }
            break;

        case WebUIState::SendAPIVer:
            if (webuiI2CSend(I2C_SERVER_API_VERSION, WEBUI_I2C_API_VERSION " ZuluSCSI"))
            {
                dbgmsg("WebUI: sent server API version ", WEBUI_I2C_API_VERSION " ZuluSCSI");
                g_wait_start_ms = ms_now();
                g_state = WebUIState::WaitAPIVer;
            }
            break;

        case WebUIState::WaitAPIVer:
        {
            bool got = webuiI2CReceive(&rx_cmd, rx_payload, &rx_length);
            if (got && rx_cmd == I2C_CLIENT_API_VERSION)
            {
                const char *client_ver = (rx_length > 0) ? (const char *)rx_payload : "(unknown)";
                // Using versioning scheme "x.y.z" match I2C client and ZuluSCSI major version "x"
                bool major_version_match = false;
                if (client_ver[0] != '\0')
                {
                    unsigned long local_major_version;
                    char* period_location = strchr(client_ver, '.');
                    if (period_location != NULL)
                    {
                        std::string remoteVersionString = client_ver;
                        unsigned long remoteMajorVersion = strtoul(client_ver, &period_location, 10);
                        period_location = strchr(WEBUI_I2C_API_VERSION, '.');
                        if (period_location != NULL)
                        {
                            unsigned long local_major_version = strtoul(WEBUI_I2C_API_VERSION, &period_location, 10);
                            if (local_major_version > 0 && local_major_version == remoteMajorVersion)
                            {
                                major_version_match = true;
                            }
                        }
                    }
                }
                if (major_version_match)
                {
                    if (strcasecmp(client_ver, WEBUI_I2C_API_VERSION) == 0)
                    {
                        logmsg("WebUI: I2C Client and ZuluSCSI API version are an exact match: ", WEBUI_I2C_API_VERSION);
                    }
                    else
                    {
                        logmsg("WebUI: I2C Client API version: ", client_ver, " ZuluSCSI API version: ", WEBUI_I2C_API_VERSION, " major version match, consider updating to latest version of firmware");
                    }
                }
                else 
                {
                    logmsg("WebUI: I2C Client API version: ", client_ver, " and ZuluSCSI API version ", WEBUI_I2C_API_VERSION, " have a major version mismatch.");
                    logmsg("=== Please upgrade both devices to the latest firmware ===");
                }

                g_state = WebUIState::SendSSID;
            }
            else if (elapsed(g_wait_start_ms, WAIT_TIMEOUT_MS))
            {
                logmsg("WebUI: timeout waiting for board API version, retrying");
                g_state = WebUIState::SendReset;
            }
            break;
            }

        case WebUIState::SendSSID:
            if (webuiI2CSend(I2C_SERVER_SSID, g_ssid))
            {
                logmsg("WebUI: sent SSID: \"", g_ssid, "\"");
                g_state = WebUIState::SendPassword;
            }
            break;

        case WebUIState::SendPassword:
            if (webuiI2CSend(I2C_SERVER_SSID_PASS, g_pass))
            {
                dbgmsg("WebUI: sent WiFi password");
                g_state = g_has_static_ip ? WebUIState::SendStaticIP : WebUIState::SendConnect;
            }
            break;

        case WebUIState::SendStaticIP:
        {
            // Send IP, netmask, and gateway with their prefixes
            char ip_msg[24], nm_msg[24], gw_msg[24];
            snprintf(ip_msg, sizeof(ip_msg), "ip%s", g_static_ip);
            snprintf(nm_msg, sizeof(nm_msg), "nm%s", g_netmask);
            snprintf(gw_msg, sizeof(gw_msg), "gw%s", g_gateway);
            webuiI2CSend(I2C_SERVER_STATIC_IP, ip_msg);
            webuiI2CSend(I2C_SERVER_STATIC_IP, nm_msg);
            webuiI2CSend(I2C_SERVER_STATIC_IP, gw_msg);
            dbgmsg("WebUI: sent static IP config");
            g_state = WebUIState::SendConnect;
            break;
        }

        case WebUIState::SendConnect:
            if (webuiI2CSend(I2C_SERVER_WIFI_CONNECT))
            {
                dbgmsg("WebUI: sent wifi connect command - I2C client board will now connect");
                g_wait_start_ms = ms_now();
                g_state = WebUIState::WaitIP;
            }
            break;

        case WebUIState::WaitIP:
        {
            // WebUI board is connecting; poll for its IP address message
            bool got = webuiI2CReceive(&rx_cmd, rx_payload, &rx_length);
            if (got && rx_cmd == I2C_CLIENT_IP_ADDRESS)
            {
                rx_payload[rx_length] = '\0';
                logmsg("WebUI: board connected, IP: ", (const char *)rx_payload);
                webuiI2CSend(I2C_SERVER_IP_ADDRESS_ACK);
                g_state = WebUIState::Running;
            }
            else if (got && rx_cmd != I2C_CLIENT_NOOP)
            {
                // Handle any other messages during WiFi connect phase
                handle_client_message(rx_cmd, rx_payload, rx_length);
            }
            else if (elapsed(g_wait_start_ms, 60000))
            {
                logmsg("WebUI: timeout waiting for board IP address, retrying handshake");
                g_state = WebUIState::SendReset;
            }
            break;
        }

        case WebUIState::Running:
        {
            // Poll for messages from WebUI board
            bool got = webuiI2CReceive(&rx_cmd, rx_payload, &rx_length);
            if (!webuiI2CWebUIPresent())
            {
                logmsg("WebUI: board lost, searching again");
                g_state = WebUIState::Searching;
                g_subscribed = false;
                break;
            }

            if (got && rx_cmd != I2C_CLIENT_NOOP)
            {
                handle_client_message(rx_cmd, rx_payload, rx_length);
            }

            // Send periodic status updates when subscribed
            if (g_subscribed && elapsed(g_last_status_ms, STATUS_INTERVAL_MS))
            {
                send_status_json();
                uint8_t sd_payload[1] = {g_sd_ready ? I2C_SERVER_SD_PRESENT : I2C_SERVER_SD_NOT_PRESENT};
                webuiI2CSend(I2C_SERVER_SD_STATUS_CHANGE, sd_payload, 1);
                g_last_status_ms = ms_now();
            }
            break;
        }

        default:
            break;
    }
}
#endif