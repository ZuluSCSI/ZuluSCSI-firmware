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

#ifndef UI_H
#define UI_H

// #define G_LOGGER // This is a hack. I needed to see logs even when it wasn't meant to be logging. 
                 // Not sure how logs are disabled when in RAW SD mode, but a better approach would be a #define
                 // to prevent the suppression of logs

// #define SCREEN_SHOTS

#include "scsi2sd.h" 
#include "ZuluSCSI_config.h" 

#define MAX_PATH_LEN MAX_FILE_PATH
#define MAX_CATEGORIES 10
#define MAX_CATEGORY_NAME_LEN 32

extern "C" void scsiReinitComplete();
extern "C" void sdCardStateChanged(bool sdAvailable);

extern "C" void controlLoop();
extern "C" void mscMode();
extern "C" void loadImage();      // in ZuluSCSI_disk used in ZuluSCSI

extern "C" void setFolder(int target_idx, bool userSet, const char *path);
extern "C" void setCurrentFolder(int target_idx, const char *path);
extern "C" void initUIDisplay();
extern "C" void initUIPostSDInit(bool cardPresent);

extern "C" void initScreens();

extern bool g_controlBoardEnabled;

#if defined(CONTROL_BOARD)

extern int g_totalCategories[S2S_MAX_TARGETS];
extern char g_categoryCodeAndNames[S2S_MAX_TARGETS][MAX_CATEGORIES][MAX_CATEGORY_NAME_LEN];
extern char g_filenameToLoad[MAX_PATH_LEN];
extern int g_pendingLoadComplete;
extern int g_pendingLoadIndex;

#endif 



// Initiator
extern bool g_initiatorMessageToProcess;

extern void UIInitiatorScanning(uint8_t deviceId, uint8_t initiatorId);
extern void UIInitiatorReadCapOk(uint8_t deviceId, S2S_CFG_TYPE deviceType, uint64_t sectorCount, uint32_t sectorSize);
extern void UIInitiatorProgress(uint8_t deviceId, uint32_t blockTime, uint32_t sectorsCopied, uint32_t sectorInBatch);
extern void UIInitiatorRetry(uint8_t deviceId);
extern void UIInitiatorSkippedSector(uint8_t deviceId);
extern void UIInitiatorTargetFilename(uint8_t deviceId, char *filename);
extern void UIInitiatorFailedToTransfer(uint8_t deviceId);
extern void UIInitiatorImagingComplete(uint8_t deviceId);

extern void UIRomCopyInit(uint8_t deviceId, S2S_CFG_TYPE deviceType, uint64_t blockCount, uint32_t blockSize, const char *filename);
extern void UIRomCopyProgress(uint8_t deviceId, uint32_t blockTime, uint32_t blockCopied);

extern void UIKioskCopyInit(uint8_t deviceIndex, uint8_t totalDevices, uint64_t blockCount, uint32_t blockSize, const char *filename);
extern void UIKioskCopyProgress(uint32_t blockTime, uint32_t blockCopied);

extern void UICreateInit(uint64_t blockCount, uint32_t blockSize, const char *filename);
extern void UICreateProgress(uint32_t blockTime, uint32_t blockCopied);

#endif
