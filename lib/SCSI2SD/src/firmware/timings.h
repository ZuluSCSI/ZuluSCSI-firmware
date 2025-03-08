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
#ifndef ZULUSCSI_SCSI2SD_TIMINGS_H
#define ZULUSCSI_SCSI2SD_TIMINGS_H
#include <stdint.h>
extern uint8_t g_max_sync_20_period;
extern uint8_t g_max_sync_10_period;
extern uint8_t g_max_sync_5_period;
extern uint8_t g_force_sync;
extern uint8_t g_force_offset;
#endif // ZULUSCSI_SCSI2SD_TIMINGS_H
