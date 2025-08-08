#if defined(CONTROL_BOARD) && !defined(ENABLE_AUDIO_OUTPUT_SPDIF)

#ifndef ABOUTSCREEN_H
#define ABOUTSCREEN_H

#include "Screen.h"

class AboutScreen : public Screen
{
public:
    AboutScreen(Adafruit_SSD1306 &display) : Screen(display) {}

    void draw();
    void shortUserPress();
};

#endif

#endif