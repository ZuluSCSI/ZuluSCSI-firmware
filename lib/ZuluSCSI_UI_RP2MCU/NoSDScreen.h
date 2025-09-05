#if defined(CONTROL_BOARD)

#ifndef NOSDSCREEN_H
#define NOSDSCREEN_H

#include "Screen.h"

class NoSDScreen : public Screen
{
public:
    NoSDScreen(Adafruit_SSD1306 *display) : Screen(display) {}

    SCREEN_TYPE screenType() { return SCREEN_NOSD; }

    void draw();
};

#endif

#endif