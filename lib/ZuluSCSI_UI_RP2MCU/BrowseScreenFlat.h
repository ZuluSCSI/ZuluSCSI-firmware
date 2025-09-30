/**
 * Copyright (c) 2025 Guy Taylor
 *
 * ZuluSCSI™ firmware is licensed under the GPL version 3 or any later version.
 *
 * https://www.gnu.org/licenses/gpl-3.0.html
 * ----
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
**/

#if defined(CONTROL_BOARD)

#ifndef BROWSESCREENFLAT_H
#define BROWSESCREENFLAT_H

#include "Screen.h"
#include "control_global.h"

class BrowseScreenFlat : public Screen
{
public:
    BrowseScreenFlat(Adafruit_SSD1306 *display) : Screen(display) { _currntBrowseScreenType = -1; }

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

    char _currentObjectDisplayName[MAX_PATH_LEN];
    NAV_OBJECT_TYPE _currentObjectType;
    
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