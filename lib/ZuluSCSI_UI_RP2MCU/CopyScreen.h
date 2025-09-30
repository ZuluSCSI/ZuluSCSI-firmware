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

#ifndef COPYSCREEN_H
#define COPYSCREEN_H

#include "ui.h"
#include "Screen.h"
#include "ProgressBar.h"

/*
To use this class, the following needs to be set:

Init
----
deviceMap->DeviceType = deviceType;
deviceMap->SectorCount = sectorCount;
deviceMap->SectorSize = sectorSize;

Updates
-------
g_copyData.NeedsProcessing
g_copyData.BlockTime;
g_copyData.SectorsCopied;
g_copyData.SectorsInBatch;

*/

class CopyScreen : public Screen
{
public:
    CopyScreen(Adafruit_SSD1306 *display) : Screen(display) 
    {
        _progressBar.graph = _display;
    }

    SCREEN_TYPE screenType() { return SCREEN_COPY; }

    void init(int index);
    void draw();
    void tick();

    void shortUserPress();

    void setBannerText(const char *text);
    void setInfoText(const char *text);
    void setShowRetriesAndErrors(bool showRetriesAndErrors);
    void setShowInfoText(bool showInfoText);

    uint8_t DeviceType;
    int BlockSize;
    uint64_t BlockCount;

    // Info
    int TotalRetries; 
    int TotalErrors;

    // Transient
    uint32_t BlockTime;
    uint32_t BlocksCopied;
    int BlocksInBatch;

    bool NeedsProcessing;
    
private:
    char _bannerText[32];
    char _infoText[MAX_PATH_LEN];
    char _buff[32];
    bool _showInfoText;
    bool _showRetriesAndErrors;

    int _scsiId;

    int _prevPer;

    long _startTime;

    bool _firstBlock;
    bool _firstTick;

    absolute_time_t _nextScreenUpdate;

    float _per;
    ProgressBar _progressBar;

#ifdef SCREEN_SHOTS
    bool _screenShotTaken;
#endif
};

extern CopyScreen *_copyScreen;

#endif

#endif