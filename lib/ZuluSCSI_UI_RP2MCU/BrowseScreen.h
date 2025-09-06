#if defined(CONTROL_BOARD)

#ifndef BROWSESCREEN_H
#define BROWSESCREEN_H

#include "Screen.h"

class BrowseScreen : public Screen
{
public:
    BrowseScreen(Adafruit_SSD1306 *display) : Screen(display) {}

    SCREEN_TYPE screenType() { return SCREEN_BROWSE; }

    void init(int index);
    void draw();
    void sdCardStateChange(bool cardIsPresent);

    void shortRotaryPress();
    void longRotaryPress();
    void shortUserPress();
    void shortEjectPress();
    void rotaryChange(int direction);

private:
    const char *_back = "..";
    
    int _scsiId;
    DeviceMap *_deviceMap;

    char _currentObjectName[MAX_PATH_LEN];
    char _currentObjectPath[MAX_PATH_LEN];
    u_int64_t _currentObjectSize;
    bool _isCurrentObjectADir;
    int _currentObjectIndex; // index into _totalObjects
    int _totalObjects;  // Real file total - doesn't include ".." if not root

    int _stackDepth;
    int _returnStack[16];
    
    // UI
    void getCurrentFilenameAndUpdateScrollers();
    void setFolderAndUpdateScrollers(char *folder, int initialFile);
    
    int navigateToNextObject();
    int navigateToPreviousObject();
    int selectCurrentObject();

    // Backend
    bool goBackADirectory();
    void setFolder(char *folder, int initialFile);
    int totalNavigationObjects();
    void getCurrentFilename();
    bool isCurrentFolderRoot();
    void loadSelectedImage();
    
    // Debug
    void printStack();
};

#endif

#endif