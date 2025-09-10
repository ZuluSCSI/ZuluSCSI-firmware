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

#include "InfoScreen.h"
#include "ZuluSCSI_log.h"

void InfoScreen::init(int index)
{
  Screen::init(index);

  _scsiId = index;

  // Init the scroller
  
  initScrollers(2);

  setupScroller(0, 42, 22, 88, 8, 1);
  setupScroller(1, 42, 36, 88, 8, 1);

  // Set the scroller text
  DeviceMap &map = g_devices[_scsiId];
  setScrollerText(0, map.Filename);
  setScrollerText(1, map.Path);
}

void InfoScreen::draw()
{ 
  _display->setCursor(0,0);             
  _display->print(F("Info"));
  
  _iconX = 92;

  DeviceMap &map = g_devices[_scsiId];

  
  const uint8_t *deviceIcon = getIconForType(map.DeviceType, true);
  drawIconFromRight(deviceIcon, 6, 0);

    if (map.IsRaw)
    {
      drawIconFromRight(icon_raw, 0, 0);
    }
    if (map.IsRom)
    {
      drawIconFromRight(icon_rom, 0, 0);
    }

    _display->drawLine(0,10,_iconX+11,10, 1);

  _display->setTextSize(2);             
  _display->setCursor(112,0);       
  _display->print(_scsiId);           
  _display->setTextSize(1);             

  _display->setCursor(0,22);             
  _display->print(F("File: "));

  _display->setCursor(0,36);             
  _display->print(F("Folder: "));

  _display->setCursor(0,46);             
  _display->print(F("Browse: "));
  _display->setCursor(42,46);       
  _display->print((map.BrowseMethod != BROWSE_METHOD_NOT_BROWSABLE)?"Yes":"No");      

  _display->setCursor(0,56);             
  _display->print(F("Size: "));
  _display->setCursor(42,56);    
  
  makeImageSizeStr(map.Size, _sizeBuffer);
  _display->print(_sizeBuffer);
}

void InfoScreen::shortUserPress()
{
  changeScreen(SCREEN_MAIN, -1);
}

#endif