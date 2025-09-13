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

#ifndef BROWSETYPESCREEN_H
#define BROWSETYPESCREEN_H

#include "ScreenList.h"

class BrowseTypeScreen : public ScreenList
{
public:
    BrowseTypeScreen(Adafruit_SSD1306 *display) : ScreenList(display) {}

    SCREEN_TYPE screenType() { return SCREEN_BROWSE_TYPE; }

    void init(int index);
    void draw();

    //void rotaryChange(int direction);
    void shortRotaryPress();
    void shortUserPress();
    void shortEjectPress();

private:
    int _scsiId;
    DeviceMap *_deviceMap;

    bool _showFolder;

    void drawItem(int x, int y, int index);
};

#endif

#endif