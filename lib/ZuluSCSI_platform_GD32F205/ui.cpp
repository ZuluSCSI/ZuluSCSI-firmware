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

InitiatorTransientData g_initiatorTransientData;

void UIInitiatorScanning(uint8_t deviceId) {}
void UIInitiatorReadCapOk(uint8_t deviceId, uint8_t deviceType, uint64_t sectorCount, uint32_t sectorSize) {}
void UIInitiatorProgress(uint8_t deviceId, uint32_t blockTime, uint32_t sectorsCopied, uint32_t sectorInBatch) {}
void UIInitiatorRetry(uint8_t deviceId) {}
void UIInitiatorSkippedSector(uint8_t deviceId) {}
void UIInitiatorTargetFilename(uint8_t deviceId, char *filename) {}
void UIInitiatorFailedToTransfer(uint8_t deviceId) {}
void UIInitiatorImagingComplete(uint8_t deviceId) {}
