#if !defined(CONTROL_BOARD) || defined(ENABLE_AUDIO_OUTPUT_SPDIF)

#include "..\src\ui.h"

extern "C" void scsiReinitComplete() {}
extern "C" void sdCardStateChanged(bool absent) {}
extern "C" void controlInit() {}
extern "C" void controlLoop() {}
extern "C" void setFolder(int target_idx, bool userSet, const char *path) {}
extern "C" void setCurrentFolder(int target_idx, const char *path) {}
extern "C" void initUI() {}

bool g_controlBoardEnabled = false;
int g_pendingLoadIndex;
int g_totalCategories[8];
char g_categoryCodeAndNames[8][10][32];
char g_filenameToLoad[MAX_PATH_LEN];
int g_pendingLoadComplete;

void UIInitiatorScanning(uint8_t deviceId) {}
void UIInitiatorReadCapOk(uint8_t deviceId, S2S_CFG_TYPE deviceType, uint64_t sectorCount, uint32_t sectorSize) {}
void UIInitiatorProgress(uint8_t deviceId, uint32_t blockTime, uint32_t sectorsCopied, uint32_t sectorInBatch) {}
void UIInitiatorRetry(uint8_t deviceId) {}
void UIInitiatorSkippedSector(uint8_t deviceId) {}
void UIInitiatorTargetFilename(uint8_t deviceId, char *filename) {}
void UIInitiatorFailedToTransfer(uint8_t deviceId) {}
void UIInitiatorImagingComplete(uint8_t deviceId) {}

void UIRomCopyInit(uint8_t deviceId, S2S_CFG_TYPE deviceType, uint64_t blockCount, uint32_t blockSize) {}
void UIRomCopyProgress(uint8_t deviceId, uint32_t blockTime, uint32_t blocksCopied) {}

void UIKioskCopyInit(uint8_t deviceIndex, uint8_t totalDevices, uint64_t blockCount, uint32_t blockSize, const char *filename) {}
void UIKioskCopyProgress(uint32_t blockTime, uint32_t blockCopied) {}

void UICreateInit(uint64_t blockCount, uint32_t blockSize, const char *filename) {}
void UICreateProgress(uint32_t blockTime, uint32_t blockCopied) {}

#endif