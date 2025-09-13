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

#ifndef SCREENLIST_H
#define SCREENLIST_H

#include "control.h"
#include "Screen.h"

#define MAX_LINES 5

class ScreenList : public Screen
{
 
public:
    ScreenList(Adafruit_SSD1306 *display) : Screen(display) {}
    
protected:
	void virtual rotaryChange(int direction);
	
	void virtual init(int index);
	void virtual draw();

	void virtual drawItem(int x, int y, int index);

	int _selectedItem;
	int _totalItems;

private:
	int _cursorPos;
    int _screenOffset;
	
};

#endif

#endif