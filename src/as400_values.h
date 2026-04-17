/** 
 * ZuluSCSI™ - Copyright (c) 2025 Rabbit Hole Computing™
 * Copyright (c) 2025 Kevin Moonlight <me@yyzkevin.com> 
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

#include <ZuluSCSI_platform_config.h>
#include <stdint.h>
#include <stddef.h>

#ifdef PLATFORM_AS400_FC6817
extern const uint8_t AS400VendorInquiry[];
extern const size_t AS400VendorInquiryLen;

extern const uint8_t AS400VitalPages[][255];
extern const size_t  AS400VitalPagesLen;

extern const uint8_t as400_mode_sense_all_pages[];
extern const size_t as400_mode_sense_all_pages_len;

extern const uint8_t as400_log_sense_page_00[];
extern const size_t as400_log_sense_page_00_len;

extern const uint16_t as400_log_sense_page_30_page_length;
extern const uint8_t  as400_log_sense_page_30_page_list_length;
extern const uint16_t as400_log_sense_page_31_page_length;

extern uint32_t as400_read_ops;
extern uint32_t as400_write_ops;

# ifdef __cplusplus
extern "C" {
# endif
void as400_get_serial_8(uint8_t scsi_id, uint8_t* serial_buf);
# ifdef __cplusplus
}
# endif

#endif