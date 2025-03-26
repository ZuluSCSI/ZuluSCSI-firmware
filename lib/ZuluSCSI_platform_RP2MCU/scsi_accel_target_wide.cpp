/**
 * ZuluSCSI™ - Copyright (c) 2022-2025 Rabbit Hole Computing™
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
**/

#ifdef RP2MCU_SCSI_ACCEL_WIDE

#include "ZuluSCSI_platform.h"
#include "ZuluSCSI_log.h"
#include "scsi_accel_target.h"
#include "timings_RP2MCU.h"
#include <hardware/pio.h>
#include <hardware/dma.h>
#include <hardware/irq.h>
#include <hardware/structs/iobank0.h>
#include <hardware/sync.h>
#include <pico/multicore.h>

void scsi_accel_rp2040_init()
{
}

void scsi_accel_log_state()
{
}

bool scsi_accel_rp2040_setSyncMode(int syncOffset, int syncPeriod)
{
    return false;
}

void scsi_accel_rp2040_startWrite(const uint8_t* data, uint32_t count, volatile int *resetFlag)
{
}

bool scsi_accel_rp2040_isWriteFinished(const uint8_t* data)
{
    return false;
}

void scsi_accel_rp2040_finishWrite(volatile int *resetFlag)
{
}

void scsi_accel_rp2040_startRead(uint8_t *data, uint32_t count, int *parityError, volatile int *resetFlag)
{
}

bool scsi_accel_rp2040_isReadFinished(const uint8_t* data)
{
    return false;
}

void scsi_accel_rp2040_finishRead(const uint8_t *data, uint32_t count, int *parityError, volatile int *resetFlag)
{
}

#endif /* RP2MCU_SCSI_ACCEL_WIDE */
