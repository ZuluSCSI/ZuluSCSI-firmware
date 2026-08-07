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

// Low-level I2C master send/receive for ZuluSCSI WebUI.
// Uses the same TwoWire (g_wire) instance as the ZuluSCSI_UI_RP2MCU library.
#ifdef ZULUCONTROL_FIRMWARE

#include "ZuluSCSI_WebUI_I2CServer.h"
#include <Wire.h>
#include <ZuluSCSI_log.h>
#include <ZuluSCSI_settings.h>
#include <string.h>

// Shared with ZuluSCSI_UI_RP2MCU (control.cpp defines it on i2c1 + GPIO_I2C_SDA/SCL)
extern TwoWire g_wire;

// Defined in ZuluSCSI_platform.cpp; set true when I2C bus is claimed by WebUI client
extern bool g_i2c_claimed;

static bool g_webui_present = false;

bool webuiI2CWebUIPresent()
{
    return g_webui_present;
}
extern uint32_t g_i2c_bus_speed;
void webuiI2CDetectClient()
{
    {
        g_wire.begin();

        if (STANDARD_I2C_CLK_SPEED != g_i2c_bus_speed)
        {
            g_i2c_bus_speed = FAST_I2C_CLK_SPEED;
            g_wire.setClock(g_i2c_bus_speed);
        }
        uint32_t test_i2c_bus_speed = g_i2c_bus_speed;
        g_wire.setClock(test_i2c_bus_speed);

        while(true)
        {
            g_wire.beginTransmission(WEBUI_I2C_SLAVE_ADDR);
            if (g_wire.endTransmission() == 0)
            {
                if (test_i2c_bus_speed == STANDARD_I2C_CLK_SPEED && g_i2c_bus_speed != test_i2c_bus_speed)
                    logmsg("I2C successfully fell back to normal bus speed (100kHz)");
                g_i2c_bus_speed = test_i2c_bus_speed;
                g_i2c_claimed = true;
                logmsg("WebUI I2C: client detected at ", (uint8_t)WEBUI_I2C_SLAVE_ADDR, ", I2C bus claimed");
                break;
            }
            else
            {
                if (test_i2c_bus_speed == STANDARD_I2C_CLK_SPEED)
                {
                    if (!g_i2c_claimed)
                    {
                        g_wire.end();
                    }
                    dbgmsg("WebUI I2C: client not detected");
                    break;
                }
                else
                {
                    // retry at the standard clockspeed
                    test_i2c_bus_speed = STANDARD_I2C_CLK_SPEED;
                    g_wire.setClock(test_i2c_bus_speed);
                }
            }
        }
    }
}

bool webuiI2CSend(uint8_t command, const uint8_t *payload, uint16_t length)
{
    if (length > WEBUI_MAX_MSG_SIZE)
    {
        logmsg("WebUI I2C: send truncated from ", (int)length, " to ", WEBUI_MAX_MSG_SIZE);
        length = WEBUI_MAX_MSG_SIZE;
    }

    g_wire.beginTransmission(WEBUI_I2C_SLAVE_ADDR);
    g_wire.write(command);
    g_wire.write((uint8_t)(length >> 8));
    g_wire.write((uint8_t)(length & 0xFF));
    if (length > 0 && payload != nullptr)
    {
        g_wire.write(payload, (size_t)length);
    }
    uint8_t result = g_wire.endTransmission();
    if (result != 0)
    {
        g_webui_present = false;
        return false;
    }
    g_webui_present = true;
    return true;
}

bool webuiI2CSend(uint8_t command, const char *str)
{
    uint16_t len = str ? (uint16_t)strlen(str) : 0;
    return webuiI2CSend(command, (const uint8_t *)str, len);
}

bool webuiI2CReceive(uint8_t *cmd_out, uint8_t *payload_out, uint16_t *length_out)
{
    *cmd_out = I2C_CLIENT_NOOP;
    *length_out = 0;
    if (payload_out) payload_out[0] = '\0';

    // Read 3-byte header: [command][length_hi][length_lo]
    uint8_t got = g_wire.requestFrom((uint8_t)WEBUI_I2C_SLAVE_ADDR, (uint8_t)3);
    if (got < 3)
    {
        // I2C WebUI client not present or not responding
        g_webui_present = (got > 0);
        return false;
    }
    g_webui_present = true;

    *cmd_out = g_wire.read();
    uint8_t len_hi = g_wire.read();
    uint8_t len_lo = g_wire.read();
    *length_out = ((uint16_t)len_hi << 8) | len_lo;

    if (*cmd_out == I2C_CLIENT_NOOP)
    {
        *length_out = 0;
        return true;
    }

    if (*length_out == 0)
    {
        return true;
    }

    if (*length_out > WEBUI_MAX_MSG_SIZE)
    {
        logmsg("WebUI I2C: received oversized message length ", (int)*length_out, ", ignoring");
        *length_out = 0;
        *cmd_out = I2C_CLIENT_NOOP;
        return false;
    }

    // Read payload in 8-byte chunks to match ZuluControl's BUFFER_LENGTH burst size.
    // ZuluControl's I2C_SLAVE_REQUEST handler sends at most 8 bytes per call and
    // deletes the outgoing packet once the final ≤8-byte burst is written. If the
    // master reads a larger chunk that spans that final burst boundary, ZuluControl
    // writes and deletes the packet while the master only reads part of it, losing
    // the remaining bytes to a NACK. Using 8-byte reads here keeps each master read
    // aligned to exactly one ZuluControl burst, preventing that off-by-one loss.
    uint16_t remaining = *length_out;
    uint8_t *ptr = payload_out ? payload_out : nullptr;

    while (remaining > 0)
    {
        uint8_t chunk = (remaining > 8) ? 8 : (uint8_t)remaining;
        uint8_t rx = g_wire.requestFrom((uint8_t)WEBUI_I2C_SLAVE_ADDR, chunk);
        if (rx < chunk)
        {
            logmsg("WebUI I2C: short payload read (", (int)rx, " of ", (int)chunk, ")");
            break;
        }
        for (uint8_t i = 0; i < rx; i++)
        {
            uint8_t b = g_wire.available() ? g_wire.read() : 0;
            if (ptr) *ptr++ = b;
        }
        remaining -= rx;
    }

    if (payload_out) payload_out[*length_out] = '\0';
    return true;
}
#endif