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

#ifndef MESSAGEBOX_H
#define MESSAGEBOX_H

#include "Screen.h"

class MessageBox : public Screen
{
public:
    MessageBox(Adafruit_SSD1306 *display) : Screen(display) {}

    SCREEN_TYPE screenType() { return MESSAGE_BOX; }

    void init(int index);
    void draw();
    void tick();
    
    void shortRotaryPress();
    void shortUserPress();
    void shortEjectPress();
    void rotaryChange(int direction);
    
    void setReturnScreen(SCREEN_TYPE ret);
    void setReturnConditionPendingLoadComplete();

    void setText(const char *title, const char *line1, const char *line2);

    bool clearScreenOnDraw();

    void ShowModal(int index, const char *title, const char *line1, const char *line2);
    
    void setBlockingMode(bool blocking);
private:
    bool _isActive;

    bool _blocking;

    int _index;
    bool _conditionPendingLoadComplete;
    SCREEN_TYPE _return;
    const char *_title;
    const char *_line1;
    const char *_line2;

    void drawText(Rectangle bound, int y, const char *msg);
};

extern MessageBox *_messageBox;

#endif

#endif