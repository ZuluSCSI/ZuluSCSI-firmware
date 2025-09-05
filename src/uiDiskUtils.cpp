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