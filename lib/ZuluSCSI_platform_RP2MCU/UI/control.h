#if defined(CONTROL_BOARD) && !defined(ENABLE_AUDIO_OUTPUT_SPDIF)

#ifndef CONTROL_H
#define CONTROL_H

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "..\src\ui.h"
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
    INITIATOR_DRIVE_PROBING,
    INITIATOR_DRIVE_CLONABLE,
    INITIATOR_DRIVE_CLONED,

} INITIATOR_DRIVE_STATUS;

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


    // Computed
    BROWSE_METHOD BrowseMethod;

    int MaxImgX;
    
    // Runtime for browsing
    char Path[MAX_PATH_LEN];
  
    // Cache
    int TotalFlatFiles;
    int HasDirs;
    int TotalFilesInCategory[MAX_CATEGORIES];
    int BrowseScreenType; // GT TODO This is really a UI concept - move?

    // Used by both Normal and Initiator
    char Filename[MAX_PATH_LEN]; 
    S2S_CFG_TYPE DeviceType;

    // Initiator
    // Reuse: uint8_t DeviceType;
    INITIATOR_DRIVE_STATUS InitiatorDriveStatus;
    int TotalRetries; 
    int TotalErrors;

    int SectorSize;
    u_int64_t SectorCount;
};

extern char g_tmpFilename[MAX_PATH_LEN];
extern char g_tmpFilepath[MAX_PATH_LEN];
extern DeviceMap g_devices[TOTAL_DEVICES];

extern const char* typeToShortString(S2S_CFG_TYPE type);
extern bool loadImageDeferred(uint8_t id, const char* next_filename, SCREEN_TYPE returnScreen, int returnIndex);
extern void patchDevice(uint8_t i);

// In UIContainer.cpp

typedef enum
{
    INITIATOR_SCANNING,
    INITIATOR_CLONING,
} INITIATOR_MODE;

extern INITIATOR_MODE g_initiatorMode;

extern void sendSDCardStateChangedToScreens(bool cardIsPresent);
extern void changeScreen(SCREEN_TYPE type, int index);


#endif

#endif