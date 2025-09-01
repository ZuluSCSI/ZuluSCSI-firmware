#if defined(CONTROL_BOARD) && !defined(ENABLE_AUDIO_OUTPUT_SPDIF)

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
    CopyScreen(Adafruit_SSD1306 &display) : Screen(display) 
    {
        _progressBar.graph = &_display;
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

extern CopyScreen _copyScreen;

#endif

#endif