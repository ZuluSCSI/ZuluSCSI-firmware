#if defined(CONTROL_BOARD) && !defined(ENABLE_AUDIO_OUTPUT_SPDIF)

#ifndef NOSDSCREEN_H
#define NOSDSCREEN_H

#include "Screen.h"

class NoSDScreen : public Screen
{
public:
    NoSDScreen(Adafruit_SSD1306 &display) : Screen(display) {}

    void draw();
};

#endif

#endif