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

#include "ZuluSCSI_disk.h"
#include "ZuluSCSI_log.h"
#include <minIni.h>

#include "ui.h"

extern "C" void getImgXByIndex(uint8_t id, int index, char* buf, size_t buflen, u_int64_t &size)
{
    char section[6] = "SCSI0";
    section[4] = scsiEncodeID(id);

    char key[5] = "IMG0";
    key[3] = '0' + index;

    ini_gets(section, key, "", buf, buflen, CONFIGFILE);

    FsVolume *vol = SD.vol();
    FsFile fHandle = vol->open(buf, O_RDONLY);
    size = fHandle.size();
    fHandle.close();
}