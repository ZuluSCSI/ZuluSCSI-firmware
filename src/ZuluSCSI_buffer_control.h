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
# ifdef CONTROL_BOARD
#   define ZULUSCSI_RESERVED_SRAM_LEN (48*1024)
# else
#   define ZULUSCSI_RESERVED_SRAM_LEN (0)
# endif
#endif

/**
  * Allocate part of the SCSI buffer for other uses
  * \param length size of the object to allocate
  * \returns the memory location of the buffer allocated for an object
  *  */
uint8_t *reserve_buffer(size_t length);

/**
 * Recover the last allocation of memory
 * \param length size of the last allocation
 */
void reserve_buffer_recover_last(size_t length);

 /**
  * Checks if reserving buffer ran out of space at any point
  * \returns true if at any point a reserve buffer ran out of buffer space
  */
bool reserve_buffer_failed();