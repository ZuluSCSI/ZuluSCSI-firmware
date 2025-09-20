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

#ifndef INFOPAGE4SCREEN_H
#define INFOPAGE4SCREEN_H

#include "Screen.h"
#include "scrolling_text.h"

class InfoPage4Screen : public Screen
{
public:
    InfoPage4Screen(Adafruit_SSD1306 *display) : Screen(display) {}

    SCREEN_TYPE screenType() { return SCREEN_INFO_PAGE4; }

    void init(int index);
    void draw();

    void shortUserPress();
    void rotaryChange(int direction);

private:
    int _scsiId;

    char _cue[MAX_FILE_PATH];
    u_int64_t _cueSize;
    int _totalBins;
};

#endif

#endif
