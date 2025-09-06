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
#include <ZuluSCSI_buffer_control.h>

static bool g_overflow = false;

#ifdef ZULUSCSI_MCU_RP23XX

static uint8_t buffer[ZULUSCSI_RESERVED_SRAM_LEN];
static size_t  buffer_length =  sizeof(buffer);

uint8_t *reserve_buffer(size_t length)
{
    if (length >= buffer_length)
    {
        g_overflow = true;
        return nullptr;
    }
    buffer_length -= length;
    return &buffer[buffer_length];
}

void reserve_buffer_recover_last(size_t length)
{
    buffer_length += length;
}
#elif defined(ZULUSCSI_MCU_RP20XX)
uint8_t *reserve_buffer(size_t length)
{
    if (length >= scsiDev.dataBufLeft)
    {
        g_overflow = true;
        return nullptr;
    }
    scsiDev.dataBufLeft -= length;
    scsiDev.dataBufLen = scsiDev.dataBufLeft & ~((size_t)0x3); // nearest 32bit boundary
    return &scsiDev.data[scsiDev.dataBufLeft];
}

void reserve_buffer_recover_last(size_t length)
{
    scsiDev.dataBufLeft += length;
    scsiDev.dataBufLen = scsiDev.dataBufLeft & ~((size_t)0x3);
}
#endif

bool reserve_buffer_failed()
{
    return g_overflow;
}