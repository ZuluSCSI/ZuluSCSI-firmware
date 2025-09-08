/**
 * ZuluSCSI™ - Copyright (c) 2025 Rabbit Hole Computing™
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

#include <scsi.h>
#include <assert.h>
#include <ZuluSCSI_buffer_control.h>
#include <ZuluSCSI_log.h>

#ifdef ZULUSCSI_MCU_RP23XX

static uint8_t buffer[ZULUSCSI_RESERVED_SRAM_LEN];
static size_t  buffer_length =  sizeof(buffer);

uint8_t *reserve_buffer_align(size_t length, uint32_t bytes)
{
    if (length == 0)
        return nullptr;

    uintptr_t mask = ~(bytes - 1);

    assert(length <= buffer_length);
    buffer_length -= length;

    uint8_t *new_address = (uint8_t*)(((uintptr_t) &buffer[buffer_length]) & mask);
    uint32_t adjustment = &buffer[buffer_length] - new_address;

    assert(adjustment <= buffer_length);
    buffer_length -= adjustment;

    return new_address;
}

#elif defined(ZULUSCSI_MCU_RP20XX)

uint8_t *reserve_buffer_align(size_t length, uint32_t bytes)
{
    assert(length <= scsiDev.dataBufLeft);
    scsiDev.dataBufLeft -= length;

    uintptr_t mask = ~(bytes - 1);

    uint8_t *new_address = (uint8_t*)(((uintptr_t) &scsiDev.data[scsiDev.dataBufLeft]) & mask);
    uint32_t adjustment = &scsiDev.data[scsiDev.dataBufLeft] - new_address;

    assert(adjustment <= scsiDev.dataBufLeft);
    scsiDev.dataBufLeft -= adjustment;

    // Align the end of the scsiDev.data buffer to ZULUSCSI_BUFFER_CONTROL_BOUNDARY_MASK
    scsiDev.dataBufLen = scsiDev.dataBufLeft & ~((uintptr_t)ZULUSCSI_BUFFER_CONTROL_BOUNDARY_MASK);
    assert(scsiDev.dataBufLen > 0);

    return new_address;
}
#endif