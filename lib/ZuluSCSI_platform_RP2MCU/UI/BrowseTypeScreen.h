#if defined(CONTROL_BOARD) && !defined(ENABLE_AUDIO_OUTPUT_SPDIF)

#ifndef BROWSETYPESCREEN_H
#define BROWSETYPESCREEN_H

#include "Screen.h"

class BrowseTypeScreen : public Screen
{
public:
    BrowseTypeScreen(Adafruit_SSD1306 &display) : Screen(display) {}

    SCREEN_TYPE screenType() { return SCREEN_BROWSE_TYPE; }

    void init(int index);
    void draw();

    void rotaryChange(int direction);
    void shortRotaryPress();
    void shortUserPress();
    void shortEjectPress();

private:
    int _selectedDevice;
    int _cursorPos;
    int _screenOffset;

    int _scsiId;
    DeviceMap *_deviceMap;
    int _totCats;

    bool _showFolder;

    void drawCategory(int x, int y, int index);
};

#endif

#endif