#ifndef UI_H
#define UI_H

// #define G_LOGGER // This is a hack. I needed to see logs even when it wasn't meant to be logging. 
                 // Not sure how logs are disabled when in RAW SD mode, but a better appoach would be a #define
                 // to prevent the supression of logs

// #define SCREEN_SHOTS

#include <sys/endian.h>

#define TOTAL_DEVICES 8
#define MAX_PATH_LEN 260
#define MAX_CATEGORIES 10
#define MAX_CATEGORY_NAME_LEN 32

extern "C" void scsiReinitComplete();
extern "C" void sdCardStateChanged(bool absent);

extern "C" void controlInit();
extern "C" void controlLoop();
extern "C" void loadImage();      // in ZuluSCSI_disk used in ZuluSCSI

extern "C" void setFolder(int target_idx, bool userSet, const char *path);
extern "C" void setCurrentFolder(int target_idx, const char *path);

extern "C" void initUI();

extern bool g_controlBoardEnabled;

extern int g_pendingLoadIndex;
extern int g_totalCategories[TOTAL_DEVICES];
extern char g_categoryCodeAndNames[TOTAL_DEVICES][MAX_CATEGORIES][MAX_CATEGORY_NAME_LEN];
extern int g_pendingLoadComplete;
extern char g_filenameToLoad[MAX_PATH_LEN];

// Initiator

typedef enum
{
    INITIATOR_MSG_NONE, // set back to this once processed
    INITIATOR_MSG_SCANNING,
    INITIATOR_MSG_READCAPOK,
    INITIATOR_MSG_TARGET_FILENAME,
    INITIATOR_MSG_PROGRESS,
    INITIATOR_MSG_FAILED_TO_TRANSFER,
    INITIATOR_MSG_RETRY,
    INITIATOR_MSG_SKIPPED_SECTOR,
    INITIATOR_MSG_IMAGING_COMPLETE
} InitiatorMessageType;

// GT TODO _ Hijack this t be the transients for initiator
struct InitiatorTransientData
{
    InitiatorMessageType Type;

    uint8_t DeviceType;
    uint8_t DeviceId;

    uint64_t SectorCount;
    int SectorSize;
    char Str[64];

    /// New Transients
    uint32_t BlockTime;
    uint32_t SectorsCopied;
    int SectorsInBatch;
};

extern InitiatorTransientData g_initiatorTransientData;

extern void UIInitiatorScanning(uint8_t deviceId);
extern void UIInitiatorReadCapOk(uint8_t deviceId, uint8_t deviceType, uint64_t sectorCount, uint32_t sectorSize);
extern void UIInitiatorProgress(uint8_t deviceId, uint32_t blockTime, uint32_t sectorsCopied, uint32_t sectorInBatch);
extern void UIInitiatorRetry(uint8_t deviceId);
extern void UIInitiatorSkippedSector(uint8_t deviceId);
extern void UIInitiatorTargetFilename(uint8_t deviceId, char *filename);
extern void UIInitiatorFailedToTransfer(uint8_t deviceId);
extern void UIInitiatorImagingComplete(uint8_t deviceId);

#endif
