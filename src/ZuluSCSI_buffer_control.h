/**
 * ZuluSCSI™ - Copyright (c) 2024-2025 Rabbit Hole Computing™
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

#pragma once

#include <stdint.h>
#include <stddef.h>

// This is for MCUs that don't need to share their scsiDev.data buffer other objects,
// but need to put the objects in some other memory location, like the control board class objects
#ifndef ZULUSCSI_RESERVED_SRAM_LEN
# if defined(ZULUSCSI_MCU_RP20XX) && defined(CONTROL_BOARD) && defined(ENABLE_AUDIO_OUTPUT_SPDIF)
#   define ZULUSCSI_RESERVED_SRAM_LEN (36*1024)
# else
#   define ZULUSCSI_RESERVED_SRAM_LEN (0)
# endif
#endif

/**
  * Allocate part of the SCSI buffer for other uses, align starting address to number of bytes
  * If the length with all the with boundary adjustments ever goes passed the buffer's
  * starting address a fail assertion will occur
  * \param length size of the object to allocate
  * \param bytes number of bytes to align the returned memory address
  * \returns the memory location of the buffer allocated for an object of size length or nullptr if the length is 0
  *  */
uint8_t *reserve_buffer_align(size_t length, uint32_t bytes);

/**
 * Returns how much of the buffer is left
 * \returns buffer left in bytes
 */
size_t reserve_buffer_left();