#if defined(CONTROL_BOARD) && !defined(ENABLE_AUDIO_OUTPUT_SPDIF)

#ifndef SPLASHSCREEN_H
#define SPLASHSCREEN_H

#include "Screen.h"

class SplashScreen : public Screen
{
public:
    SplashScreen(Adafruit_SSD1306 &display) : Screen(display) {}

    void init(int index);
};

#endif

#endif