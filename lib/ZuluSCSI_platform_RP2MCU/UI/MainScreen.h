#if defined(CONTROL_BOARD) && !defined(ENABLE_AUDIO_OUTPUT_SPDIF)

#ifndef MAINSCREEN_H
#define MAINSCREEN_H

#include "Screen.h"

class MainScreen : public Screen
{
public:
    MainScreen(Adafruit_SSD1306 &display) : Screen(display) 
    {
        _selectedDevice= -1;
    }

    void init(int index);
    void draw();
    void tick();
    
    void sdCardStateChange(bool cardIsPresent);

    void shortRotaryPress();
    void shortEjectPress();
    void longUserPress();
    void longEjectPress();
    void rotaryChange(int direction);

private:
    int _selectedDevice;
    DeviceMap *_deviceMap;
    
    void drawSCSIItem(int x, int y, int index);
};

#endif

#endif