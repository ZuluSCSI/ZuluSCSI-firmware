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

#include "ScreenList.h"
#include "ZuluSCSI_log.h"

void ScreenList::init(int index)
{
  Screen::init(index);

  _selectedItem = index;
  if (_selectedItem == -1)
  {
    _selectedItem = 0;
  }

  _cursorPos = _selectedItem;
  _screenOffset = 0;
  int diff = MAX_LINES - (_selectedItem+1);
  if (diff < 0)
  {
    _cursorPos  += diff;
    _screenOffset -= diff;
  }
}

void ScreenList::drawItem(int x, int y, int index)
{
  if (_selectedItem  == index)
  {
    _display->drawBitmap(x, y, icon_select, 8,8, WHITE);
  }
}

void ScreenList::draw()
{   
  int yOffset = 14;
  int xOffset = 0;

  int i;
  for (i=0;i<MAX_LINES;i++)
  {
    int ind = i + _screenOffset;
    if (ind < _totalItems)
    {
      drawItem(xOffset, yOffset, ind);
      yOffset+=10;
    }
  }
}

void ScreenList::rotaryChange(int direction)
{
  if (_selectedItem == -1) // there aren't any, so just return (it would have been set to something other than -1 if there were)
  {
    return;
  }
  
  if (direction == 1)
  {
    _selectedItem++;
    if (_selectedItem == _totalItems)
    {
      _selectedItem--;
    }
    else
    {
      _cursorPos++;
      if (_cursorPos == MAX_LINES)
      {
        _cursorPos--;
        _screenOffset++;
      }
    }
  }
  else // dir -1
  {
    _selectedItem--;
    if (_selectedItem == -1)
    {
      _selectedItem++;
    }
    else
    {
      _cursorPos--;
      if (_cursorPos < 0)
      {
        _cursorPos++;
        _screenOffset--;
      }
    }
  }

  forceDraw();
}

#endif