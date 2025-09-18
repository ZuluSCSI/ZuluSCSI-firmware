/**
 * Copyright (c) 2025 Guy Taylor
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

#if defined(CONTROL_BOARD)

#ifndef CONTROL_GLOBAL_H
#define CONTROL_GLOBAL_H

#include "ui.h"
#include "ZuluSCSI_disk.h"
#include "SDNavigator.h"

// new in ZuluSCSI_disk.h
extern image_config_t g_DiskImages[S2S_MAX_TARGETS];

extern "C" void setPendingImageLoad(uint8_t id, const char* next_filename);
extern "C" void getImgXByIndex(uint8_t id, int index, char* buf, size_t buflen, u_int64_t &size);

// new in ZuluSCSI_log.h
extern const char *g_log_short_firmwareversion;

// new in ZuluSCSI_platform.h
extern bool g_scsi_initiator;

#endif

#endif