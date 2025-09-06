#if defined(CONTROL_BOARD)

#ifndef INITIATORMAINSCREEN_H
#define INITIATORMAINSCREEN_H

#include "Screen.h"
#include "scrolling_text.h"

class InitiatorMainScreen : public Screen
{
public:
    InitiatorMainScreen(Adafruit_SSD1306 *display) : Screen(display) {}

    SCREEN_TYPE screenType() { return SCREEN_INITIATOR_MAIN; }

    void tick();
    void init(int index);
    void draw();

private:
    int _scsiId;

    void drawSCSIItem(int x, int y, int index);
};

#endif

#endif