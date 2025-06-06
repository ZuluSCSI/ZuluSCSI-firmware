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

// SCSI subroutines that implement synchronous mode SCSI.
// Uses DMA for data transfer, EXMC for data input and
// GD32 timer for the REQ pin toggling.

#pragma once

#include <stdint.h>
#include "ZuluSCSI_platform.h"

#ifdef SCSI_IN_ACK_EXMC_NWAIT_PORT
#define SCSI_SYNC_MODE_AVAILABLE
#endif

void scsi_accel_sync_init();

void scsi_accel_sync_recv(uint8_t *data, uint32_t count, int* parityError, volatile int *resetFlag);
void scsi_accel_sync_send(const uint8_t* data, uint32_t count, volatile int *resetFlag);
