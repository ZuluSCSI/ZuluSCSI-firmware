#if defined(CONTROL_BOARD) && !defined(ENABLE_AUDIO_OUTPUT_SPDIF)

#ifndef ABOUTSCREEN_H
#define ABOUTSCREEN_H

#include "Screen.h"

class AboutScreen : public Screen
{
public:
    AboutScreen(Adafruit_SSD1306 &display) : Screen(display) {}

    SCREEN_TYPE screenType() { return SCREEN_ABOUT; }

    void draw();
    void shortUserPress();
};

#endif

#endif