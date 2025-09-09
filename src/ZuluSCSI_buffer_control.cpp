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

static uint8_t buffer[ZULUSCSI_RESERVED_SRAM_LEN];
static size_t  buffer_length =  sizeof(buffer);

uint8_t *reserve_buffer_align(size_t length, uint32_t bytes)
{
    if (length == 0)
        return nullptr;

    uintptr_t mask = ~(bytes - 1);

    if(length > buffer_length)
    {
        logmsg("Buffer Control ran out of space by ", (int)(length - buffer_length)," bytes increase ZULUSCSI_RESERVED_SRAM_LEN and recompile");
        assert(false);
    }
    buffer_length -= length;

    uint8_t *new_address = (uint8_t*)(((uintptr_t) &buffer[buffer_length]) & mask);
    size_t adjustment = &buffer[buffer_length] - new_address;
    if (adjustment > buffer_length)
    {
        logmsg("Buffer Control ran out of space after alignment by ", (int)(adjustment - buffer_length)," bytes , increase ZULUSCSI_RESERVED_SRAM_LEN and recompile");
        assert(false);
    }
    buffer_length -= adjustment;
    return new_address;
}

size_t reserve_buffer_left()
{
    return buffer_length;
}