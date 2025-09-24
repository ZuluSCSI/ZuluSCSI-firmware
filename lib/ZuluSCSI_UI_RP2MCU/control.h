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

#ifndef CONTROL_H
#define CONTROL_H

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "ui.h"
#include "ScreenType.h"
#include "dimensions.h"
#include "BrowseMethod.h"

#include <scsi2sd.h>
#include "ZuluSCSI_config.h"
#include "control_global.h"

// GPIO Expansion bus pins IO0-IO7
#define EXP_ROT_A_PIN   0
#define EXP_ROT_B_PIN   1
#define EXP_EJECT_PIN   2
#define EXP_ROT_PIN     5
#define EXP_INSERT_PIN  3

typedef enum
{
    INITIATOR_DRIVE_UNKNOWN,
    INITIATOR_DRIVE_SCANNED,
    INITIATOR_DRIVE_PROBING,
    INITIATOR_DRIVE_CLONABLE,
    INITIATOR_DRIVE_CLONED,
    INITIATOR_DRIVE_HOST

} INITIATOR_DRIVE_STATUS;

typedef enum
{
    SCREEN_BROWSETYPE_FOLDERS,
    SCREEN_BROWSETYPE_FLAT,
    SCREEN_BROWSETYPE_CATEGORY
} SCREEN_BROWSETYPES;

struct DeviceMap
{
    // Set during path checking phase
    bool Active;
    bool UserFolder; // Not used. True is an "ImgDir' folder defined, false if a named folder found e.g. 'CD0' but no "ImgDir'
    char RootFolder[MAX_PATH_LEN];

    // Set during patching
    uint64_t Size;
    bool IsRemovable;
    bool IsRaw;
    bool IsRom;
    bool IsWritable;

    uint32_t LBA;
    uint16_t BytesPerSector;
    uint16_t SectorsPerTrack;
	uint16_t HeadsPerCylinder;

    char Vendor[9];
	char ProdId[17];
	char Revision[5];
	char Serial[17];

    uint16_t Quirks;

    // Computed
    BROWSE_METHOD BrowseMethod;

    int MaxImgX;
    
    // Runtime for browsing
    char Path[MAX_PATH_LEN];
  
    // Both of these are set on load for both cahced and non cached
    int TotalFlatFiles;
    int HasDirs;

    // Cache
    int TotalFilesInCategory[MAX_CATEGORIES];
    SCREEN_BROWSETYPES BrowseScreenType; // GT TODO This is really a UI concept - move?

    char LoadedObject[MAX_PATH_LEN];  // This is the same as filename for normal object, but the folder name for cue/bin
    NAV_OBJECT_TYPE NavObjectType;

    // Cahce during initial load or loadLoad
    char CueFilename[MAX_PATH_LEN]; 
    int TotalBins;
    uint64_t CueSize;

    // Used by both Normal and Initiator
    char Filename[MAX_PATH_LEN]; 
    S2S_CFG_TYPE DeviceType;

    // Initiator
    // Reuse: uint8_t DeviceType;
    INITIATOR_DRIVE_STATUS InitiatorDriveStatus;
    int TotalRetries; 
    int TotalErrors;

    int SectorSize;
    uint64_t SectorCount;
};


extern DeviceMap *g_devices;

extern const char* typeToShortString(S2S_CFG_TYPE type);

/**
 * Keep Message box open for delay_ms millisecond
 * \param open_ms number of milliseconds to keep message box open, 0 immediately closes
 * \param screen the screen to change to after it closes
 * \param deviceId the device ID for the screen to change to
 */

extern void deferredMessageBoxClose(uint32_t open_ms, SCREEN_TYPE screen, int deviceId);
extern bool loadImageDeferred(uint8_t id, const char* next_filename, SCREEN_TYPE returnScreen, int returnIndex);
extern void patchDevice(uint8_t i);
extern void UpdateDeviceInfo(int target_idx, const char *fullPath, const char *path, const char *file, NAV_OBJECT_TYPE navObjectType);

// In UIContainer.cpp

typedef enum
{
    INITIATOR_SCANNING,
    INITIATOR_CLONING,
} INITIATOR_MODE;

extern INITIATOR_MODE g_initiatorMode;

extern void sendSDCardStateChangedToScreens(bool cardIsPresent);
extern void changeScreen(SCREEN_TYPE type, int index);

extern bool isFolderACueBinSet(const char *folder, char *cueFile, u_int64_t &cueSize, u_int64_t &binSize, int &totalBins);

extern void printDevices();

#endif

#endif