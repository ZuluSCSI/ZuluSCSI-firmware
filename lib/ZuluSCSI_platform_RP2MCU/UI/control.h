#if defined(CONTROL_BOARD) && !defined(ENABLE_AUDIO_OUTPUT_SPDIF)

#ifndef CONTROL_H
#define CONTROL_H

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "..\src\ui.h"
#include "UI/ScreenType.h"
#include "UI/dimensions.h"

#include <scsi2sd.h>
#include "ZuluSCSI_config.h"
#include "control_global.h"

// GPIO Expansion bus pins IO0-IO7
#define EXP_ROT_A_PIN   0
#define EXP_ROT_B_PIN   1
#define EXP_EJECT_PIN   2
#define EXP_ROT_PIN     5
#define EXP_INSERT_PIN  3

#define TOTAL_DEVICES 8

struct DeviceMap
{
    // Set during path checking phase
    bool Active;
    bool UserFolder;
    char RootFolder[MAX_PATH_LEN];

    // Set during patching
    uint8_t DeviceType;
    char Filename[MAX_PATH_LEN]; 
    uint64_t Size;
    bool IsRemovable;
    bool IsRaw;
    bool IsRom;
    bool IsWritable;

    // Computed
    bool IsBrowsable;  // to be browsable it must be removeable & have a user folder

    // Runtime for browsing
    char Path[MAX_PATH_LEN];
  
    // Cache
    int TotalFlatFiles;
    int HasDirs;
    int TotalFilesInCategory[MAX_CATEGORIES];
    int BrowseScreenType; // TODO This is really a UI concept - move?
};

extern char g_tmpFilename[MAX_PATH_LEN];
extern char g_tmpFilepath[MAX_PATH_LEN];
extern DeviceMap g_devices[TOTAL_DEVICES];

extern const char* typeToShortString(S2S_CFG_TYPE type);
extern bool loadImageDeferred(uint8_t id, const char* next_filename, SCREEN_TYPE returnScreen, int returnIndex);
extern void patchDevice(uint8_t i);

// In UIContainer.cpp
extern void sendSDCardStateChangedToScreens(bool cardIsPresent);
extern void changeScreen(SCREEN_TYPE type, int index);

#endif

#endif