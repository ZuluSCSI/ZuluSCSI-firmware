#if defined(CONTROL_BOARD) && !defined(ENABLE_AUDIO_OUTPUT_SPDIF)

#ifndef SPLASHSCREEN_H
#define SPLASHSCREEN_H

#include "Screen.h"
#include "SystemMode.h"

class SplashScreen : public Screen
{
public:
    SplashScreen(Adafruit_SSD1306 &display) : Screen(display) {}

    SCREEN_TYPE screenType() { return SCREEN_SPLASH; }

    void init(int index);
    void showMode(SYSTEM_MODE mode);
};

extern SplashScreen _splashScreen;

#endif

#endif