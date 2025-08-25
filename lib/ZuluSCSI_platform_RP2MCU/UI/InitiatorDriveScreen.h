#if defined(CONTROL_BOARD) && !defined(ENABLE_AUDIO_OUTPUT_SPDIF)

#ifndef INITIATORDRIVESCREEN_H
#define INITIATORDRIVESCREEN_H

#include "Screen.h"
#include "ProgressBar.h"

class InitiatorDriveScreen : public Screen
{
public:
    InitiatorDriveScreen(Adafruit_SSD1306 &display) : Screen(display) 
    {
        _progressBar.graph = &_display;
    }

    SCREEN_TYPE screenType() { return SCREEN_INITIATOR_DRIVE; }

    void init(int index);
    void draw();
    void tick();

    void shortUserPress();

private:
    char _buff[32];

    int _scsiId;

    int _prevPer;

    long _startTime;

    bool _firstBlock;
    bool _firstTick;

    absolute_time_t _nextScreenUpdate;

    float _per;
    ProgressBar _progressBar;
};

#endif

#endif