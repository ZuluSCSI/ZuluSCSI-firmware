#if defined(CONTROL_BOARD) && !defined(ENABLE_AUDIO_OUTPUT_SPDIF)

#ifndef INFOSCREEN_H
#define INFOSCREEN_H

#include "Screen.h"
#include "scrolling_text.h"

class InfoScreen : public Screen
{
public:
    InfoScreen(Adafruit_SSD1306 &display) : Screen(display) {}

    void init(int index);
    void draw();

    void shortUserPress();

private:
    int _scsiId;
};

#endif

#endif