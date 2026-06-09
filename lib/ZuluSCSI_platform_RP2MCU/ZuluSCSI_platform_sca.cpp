/**
 * ZuluSCSI™ - Copyright (c) 2022-2026 Rabbit Hole Computing™
 *
 * This file is licensed under the GPL version 3 or any later version.
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

#ifdef ZULUSCSI_WIDE

#include "ZuluSCSI_sca_hw_config.h"
#include "ZuluSCSI_log.h"
#include <ZuluSCSI_platform.h>
#include <Wire.h>

#ifndef ZULUSCSI_SCA_I2C_EXPANDER_ADDR 
#define ZULUSCSI_SCA_I2C_EXPANDER_ADDR 0x19
#endif

extern TwoWire g_wire;

struct
{
    int8_t scsi_id;
    bool delayed_start;
    bool remote_start;
    bool mated_1;
    bool valid;
    bool failed;
} g_sca_hardware_config;

bool zuluscsi_is_sca()
{
    return platform_is_sca();
}

bool zuluscsi_sca_hw_update()
{
    g_sca_hardware_config.failed = false;
    g_sca_hardware_config.valid = false;
    g_sca_hardware_config.scsi_id = -1;
    int expander_io_input = -1;
    g_wire.end();
    g_wire.setClock(PLATFORM_I2C_CLK_SPEED);
    g_wire.begin();

    // invert all inputs
    g_wire.beginTransmission(ZULUSCSI_SCA_I2C_EXPANDER_ADDR);
    g_wire.write((uint8_t) 0x02); // Control - Switch to write to Polarity Register
    g_wire.write((uint8_t) 0x7F); // Set Polarity Register - invert all bits but spindle
    uint8_t status = g_wire.endTransmission();
    if (status == 0)
    {
        g_wire.beginTransmission(ZULUSCSI_SCA_I2C_EXPANDER_ADDR);
        g_wire.write((uint8_t) 0x00); // Control - Switch to read from Input Register
        status = g_wire.endTransmission();
    } 
    if (0 != status)
    {
        g_sca_hardware_config.failed = true;
        logmsg("Error, ", (int)status,", communicating with the SCA I2C IO expander with address ", (uint8_t)ZULUSCSI_SCA_I2C_EXPANDER_ADDR, ". Check for conflicting I2C devices and a clean I2C bus");
    }

    if (!g_sca_hardware_config.failed)
    {

        g_wire.requestFrom(ZULUSCSI_SCA_I2C_EXPANDER_ADDR, 1);

        if (g_wire.available())
        {
            expander_io_input = g_wire.read();
        }

        if (expander_io_input == -1)
        {
            g_sca_hardware_config.failed = true;
            logmsg("Error reading the SCA config pin from the I2C IO expander");
            
        }
        else
        {
            g_sca_hardware_config.mated_1 = expander_io_input & 0x10;
            // should wait 250 ms after mated 1 is valid to read other values
            // ignoring for now, we'll see if that becomes an issue 

            g_sca_hardware_config.scsi_id = expander_io_input & 0xF;
            g_sca_hardware_config.delayed_start = expander_io_input & 0x20;
            g_sca_hardware_config.remote_start = expander_io_input & 0x40;
            g_sca_hardware_config.valid = g_sca_hardware_config.mated_1;
            if (g_sca_hardware_config.valid)
            {
                dbgmsg("Valid SCA settings: SCSI ID: ", (int) g_sca_hardware_config.scsi_id,
                        ", Delayed Start: ", g_sca_hardware_config.delayed_start ? "yes": "no",
                        ", Remote Start: ",  g_sca_hardware_config.remote_start  ? "yes": "no"
                );
                LED_OFF();
            }
            else
            {
                static uint32_t invalid_print_start = 0;

                if ((uint32_t)(millis() - invalid_print_start) > 3000 || invalid_print_start == 0)
                {
                    static bool led_state_on = false;
                    if (led_state_on)
                        LED_ON();
                    else
                        LED_OFF();
                    led_state_on = !led_state_on;

                    dbgmsg("Invalid SCA settings - mated 1 not ready");
                    invalid_print_start = millis();
                }
            }

            
        }
    }
    return !g_sca_hardware_config.failed;
}

int8_t zuluscsi_sca_hw_scsi_id()
{
    return g_sca_hardware_config.valid ? g_sca_hardware_config.scsi_id : -1;
}
bool zuluscsi_sca_hw_valid()
{
    return g_sca_hardware_config.valid;
}

bool zuluscsi_sca_hw_delayed_start()
{
    return g_sca_hardware_config.delayed_start;
}

bool zululscsi_sca_hw_remote_start()
{
    return g_sca_hardware_config.remote_start;
}

#endif // ZULUSCSI_WIDE
