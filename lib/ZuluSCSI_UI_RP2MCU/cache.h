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

#ifndef CACHE_H
#define CACHE_H

extern bool g_cacheActive;

extern "C" bool doesDeviceHaveAnyCategoryFiles(int scsiId);

extern "C" void getCacheFile(int scsiId, char cat, int index, char *file, char *path, u_int64_t &size);
extern "C" int findCacheFile(int scsiId, char cat, char *file, char *path);

extern void clearCacheData();
extern void buildCache();

#endif

#endif