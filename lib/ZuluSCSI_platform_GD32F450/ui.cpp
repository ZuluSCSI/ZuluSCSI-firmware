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

#include "ui.h"

extern "C" void scsiReinitComplete() {}
extern "C" void controlLoop() {}
extern "C" bool mscMode() { return false; }
extern "C" void setFolder(int target_idx, bool userSet, const char *path) {}
extern "C" void setCurrentFolder(int target_idx, const char *path) {}
extern "C" void initUIDisplay() {}
extern "C" void initUIPostSDInit(bool cardPresent) {}

bool g_controlBoardEnabled = false;

bool g_initiatorMessageToProcess;

void UIInitiatorScanning(uint8_t deviceId, uint8_t initiatorId) {}
void UIInitiatorReadCapOk(uint8_t deviceId, S2S_CFG_TYPE deviceType, uint64_t sectorCount, uint32_t sectorSize) {}
void UIInitiatorProgress(uint8_t deviceId, uint32_t blockTime, uint32_t sectorsCopied, uint32_t sectorInBatch) {}
void UIInitiatorRetry(uint8_t deviceId) {}
void UIInitiatorSkippedSector(uint8_t deviceId) {}
void UIInitiatorTargetFilename(uint8_t deviceId, char *filename) {}
void UIInitiatorFailedToTransfer(uint8_t deviceId) {}
void UIInitiatorImagingComplete(uint8_t deviceId) {}

void UIRomCopyInit(uint8_t deviceId, S2S_CFG_TYPE deviceType, uint64_t blockCount, uint32_t blockSize, const char *filename) {}
void UIRomCopyProgress(uint8_t deviceId, uint32_t blockTime, uint32_t blocksCopied) {}

void UIKioskCopyInit(uint8_t deviceIndex, uint8_t totalDevices, uint64_t blockCount, uint32_t blockSize, const char *filename) {}
void UIKioskCopyProgress(uint32_t blockTime, uint32_t blockCopied) {}

void UICreateInit(uint64_t blockCount, uint32_t blockSize, const char *filename) {}
void UICreateProgress(uint32_t blockTime, uint32_t blockCopied) {}