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

#include <scsi2sd.h>
#include "ZuluSCSI_platform.h"
#include "ZuluSCSI_disk.h"
#include "ZuluSCSI_log.h"

#include "MessageBox.h"
#include "cache.h"
#include "control_global.h"

extern SdFs SD;
FsFile g_fileHandle;
bool g_cacheActive;

int g_maxCount;
int g_currentID;
char g_tmpCacheFilepath[MAX_FILE_PATH];

extern "C" bool doesDeviceHaveAnyCategoryFiles(int scsiId)
{
    int i;
    DeviceMap *deviceMap = &g_devices[scsiId];
    for (i=0;i<g_totalCategories[scsiId];i++)
    {
        if (deviceMap->TotalFilesInCategory[i] > 0)
        {
            return true;
        }
    }
    return false;
}

FsFile appendFile(const char*filenamne)
{
    FsVolume *vol = SD.vol();

    if (SD.exists(filenamne))
    {
        return vol->open(filenamne, O_RDWR | O_APPEND | O_AT_END);
    }
    return vol->open(filenamne, O_WRONLY | O_CREAT);
}

void fileCallback(int count, const char *file, const char *path, u_int64_t size)
{
    DeviceMap *deviceMap = &g_devices[g_currentID];

    g_maxCount = count;

    char *lastOpen = strrchr(file, '{');
    char *lastClose = strrchr(file, '}');
    lastOpen+=1;

    if (lastOpen != NULL && lastClose != NULL)
    {
        while(lastOpen != lastClose)
        {
            int i;
            bool found = false;
            for (i=0;i<g_totalCategories[g_currentID];i++)
            {
                if (g_categoryCodeAndNames[g_currentID][i][0] == *lastOpen)
                {
                    deviceMap->TotalFilesInCategory[i]++;
                    
                    strcpy(g_tmpFilepath, ".cache/cache0_.dat");
                    g_tmpFilepath[13] = *lastOpen;
                    g_tmpFilepath[12] = (char)(g_currentID+48);
                    
                    FsFile catHandle = appendFile(g_tmpFilepath);

                    catHandle.write((u_int8_t *)file, MAX_PATH_LEN);
                    catHandle.write((u_int8_t *)path, MAX_PATH_LEN);
                    catHandle.write((u_int8_t *)&size, sizeof(u_int64_t));

                    catHandle.close();
                    found = true;
                }
            }
            if (!found)
            {
                logmsg("Unmatched category in filename '", file, "': ", (char)*lastOpen);
            }

            lastOpen++;
        }
    }
    g_fileHandle.write((u_int8_t *)file, MAX_PATH_LEN);
    g_fileHandle.write((u_int8_t *)path, MAX_PATH_LEN);
    g_fileHandle.write((u_int8_t *)&size, sizeof(u_int64_t));
}

void purgeCacheFolder()
{
    FsFile cacheFolder;
    FsFile file;
    if (!cacheFolder.open("/.cache")) 
    {
        logmsg("Couldn't open .cache folder");
        return;
    }

    while (file.openNext(&cacheFolder, O_RDONLY)) 
    {
        if (!file.isHidden()) 
        {
            if (!file.getName(g_tmpFilename, MAX_PATH_LEN))
            {
                logmsg("Failed top get filename in cahce folder");
            }
            else
            {
                strcpy(g_tmpFilepath, ".cache/");
                strcat(g_tmpFilepath, g_tmpFilename);

                bool res = SD.remove(g_tmpFilepath);
                if (res == 0)
                {
                    logmsg("Failed to purge file: ", g_tmpFilepath);
                }
            }
        }
        file.close();
    }
}

int GetIndexFromCategoryCode(int scsiId, char cat)
{
    int i;
    for (i=0; i<g_totalCategories[scsiId]; i++)
    {
        if (g_categoryCodeAndNames[scsiId][i][0] == cat)
        {
            return i;
        }
    }
    return -1;
}


extern "C" int findCacheFile(int scsiId, char cat, char *file, char *path)
{
    int total = 0;
    DeviceMap *deviceMap = &g_devices[scsiId];

    if (cat == '_')
    {
        total = deviceMap->TotalFlatFiles;
    }
    else
    {
        int catIndex  = GetIndexFromCategoryCode(scsiId, cat);
        total = deviceMap->TotalFilesInCategory[catIndex];
    }

    strcpy(g_tmpCacheFilepath, ".cache/cache0_.dat");
    g_tmpCacheFilepath[13] = cat;
    g_tmpCacheFilepath[12] = (char)(scsiId+48);

    FsVolume *vol = SD.vol();
    FsFile fHandle = vol->open(g_tmpCacheFilepath, O_RDONLY);

    int i;
    int blockSize = ((MAX_PATH_LEN + MAX_PATH_LEN) + sizeof(u_int64_t));

    for (i=0;i<total;i++)
    {
        int offset = blockSize * i;
        u_int64_t size;

        fHandle.seek(offset);

        fHandle.read(file, MAX_PATH_LEN);
        fHandle.read(path, MAX_PATH_LEN);
        fHandle.read(&size, sizeof(u_int64_t));

        if (strcmp(deviceMap->Filename, file) == 0 && strcmp(deviceMap->Path, path) == 0)
        {
            fHandle.close();
            return i;
        }
    }

    fHandle.close();
    return -1;
}

extern "C" void getCacheFile(int scsiId, char cat, int index, char *file, char *path, u_int64_t &size)
{
    strcpy(g_tmpCacheFilepath, ".cache/cache0_.dat");
    g_tmpCacheFilepath[13] = cat;
    g_tmpCacheFilepath[12] = (char)(scsiId+48);

    FsVolume *vol = SD.vol();
    FsFile fHandle = vol->open(g_tmpCacheFilepath, O_RDONLY);

    int blockSize = ((MAX_PATH_LEN + MAX_PATH_LEN) + sizeof(u_int64_t));
    int offset = blockSize * index;

    fHandle.seek(offset);

    fHandle.read(file, MAX_PATH_LEN);
    fHandle.read(path, MAX_PATH_LEN);
    fHandle.read(&size, sizeof(u_int64_t));

    fHandle.close();
}

// By setting this, there are implicitely no categories
void clearCacheData()
{
    int i;
    for (i = 0; i < S2S_MAX_TARGETS; i++)
    {
        DeviceMap *deviceMap = &g_devices[i];

        deviceMap->BrowseScreenType = 0;
        deviceMap->TotalFlatFiles = 0;

        int j;
        for (j=0;j<g_totalCategories[i];j++)
        {
            deviceMap->TotalFilesInCategory[j] = 0;
        }

        g_totalCategories[i] = 0;  // blank categories as we don't need them
    }
}

void buildCache()
{
    _messageBox->setText("-- Busy --", "Building", "Cache...");
    changeScreen(MESSAGE_BOX, -1);
    _messageBox->tick(); // During boot, there is no loop, so manually trigger the tick, to draw the screen

    if (!SD.exists(".cache"))
    {
        bool res = SD.mkdir(".cache");
        if (res ==0)
        {
            logmsg("Failed to create cache folder");
        }
    }

    purgeCacheFolder();

    int i;
    for (i = 0; i < S2S_MAX_TARGETS; i++)
    {
        DeviceMap *deviceMap = &g_devices[i];

        g_currentID = i;
        deviceMap->BrowseScreenType = 0;
        deviceMap->TotalFlatFiles = 0;

        int j;
        for (j=0;j<g_totalCategories[i];j++)
        {
            deviceMap->TotalFilesInCategory[j] = 0;
        }

        DeviceMap *map = &g_devices[i];

        switch(map->BrowseMethod)
        {
            case BROWSE_METHOD_NOT_BROWSABLE:
                break;

            case BROWSE_METHOD_IMDDIR:
            {
                strcpy(g_tmpFilepath, ".cache/cache0_.dat");
                g_tmpFilepath[12] = (char)(i+48);
                g_fileHandle = appendFile(g_tmpFilepath);

                bool hasDirs = false;
                SDNavScanFilesRecursive.ScanFilesRecursive(map->RootFolder, hasDirs, fileCallback); 

                g_fileHandle.close();

                deviceMap->HasDirs = hasDirs;
                
                deviceMap->TotalFlatFiles = g_maxCount+1;
                break;
            }
            case BROWSE_METHOD_IMGX:
                // No caching needed
                break;

            case BROWSE_METHOD_USE_PREFIX:
                // Assumption: file will be in the root folder (Dir and Dir(0..9) appear broken)
                // TODO - not implemented yet but is it needed? would seem like a low number of images for this style?
                break;
        }
    }
}

#endif