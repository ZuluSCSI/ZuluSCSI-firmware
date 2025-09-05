#if defined(CONTROL_BOARD) && !defined(ENABLE_AUDIO_OUTPUT_SPDIF)

#ifndef BROWSESCREENFLAT_H
#define BROWSESCREENFLAT_H

#include "Screen.h"
#include "control_global.h"

class BrowseScreenFlat : public Screen
{
public:
    BrowseScreenFlat(Adafruit_SSD1306 &display) : Screen(display) { _currntBrowseScreenType = -1; }

    SCREEN_TYPE screenType() { return SCREEN_BROWSE_FLAT; }

    void init(int index);
    void draw();

    void shortRotaryPress();
    void shortUserPress();
    void shortEjectPress();
    void rotaryChange(int direction);

    void virtual sdCardStateChange(bool cardIsPresent);

private:
    int _scsiId;
    DeviceMap *_deviceMap;

    const char *_back = "..";
    char _currentObjectName[MAX_PATH_LEN];
    char _currentObjectPath[MAX_PATH_LEN];
    u_int64_t _currentObjectSize;
    int _currentObjectIndex; // index into _totalObjects
    int _totalObjects;  // Real file total - doesn't include ".." if not root
    char _catChar;
    int _currntBrowseScreenType;

    // UI
    int navigateToNextObject();
    int navigateToPreviousObject();
    
    void getCurrentFilenameAndUpdateScrollers();

    // 
    void initImgDir(int index);
    void initImgX(int index);
    void initPrefix(int index);

    // Backend
    void getCurrentFilename();
    void loadSelectedImage();
};

#endif

#endif