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

  setupScroller(0, 52, 16, 88, 8, 1);
  setupScroller(1, 52, 26, 88, 8, 1);

  // Set the scroller text
  DeviceMap &map = g_devices[_scsiId];

  setScrollerText(0, map.LoadedObject);
  setScrollerText(1, map.Path);
}

void InfoScreen::draw()
{ 
  DeviceMap &map = g_devices[_scsiId];

  _display->setCursor(0,0);    
  _display->print(F("Info (1/3)"));         
  
  _iconX = _display->width();

  _display->setTextSize(2);
  printNumberFromTheRight(_scsiId, 6, 0);
  _display->setTextSize(1);

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


  _display->setCursor(0,16);    
  if (map.NavObjectType == NAV_OBJECT_CUE)
  {         
    _display->print(F("BinCue:"));
  }
  else
  {
    _display->print(F("File:"));
  }

  _display->setCursor(0,26);             
  _display->print(F("Folder: "));

  
  _display->setCursor(0,36);             
  _display->print(F("Size: "));
  _display->setCursor(52,36);    
  makeImageSizeStr(map.Size, _sizeBuffer);
  _display->print(_sizeBuffer);

  _display->setCursor(0,56);             
  _display->print(F("Style: "));
  _display->setCursor(52,56);   


  switch(map.BrowseMethod)
  {
    case BROWSE_METHOD_IMDDIR:
      _display->print(F("ImgDir"));
      break;

    case BROWSE_METHOD_IMGX:
      _display->print(F("ImgX"));
      break;

    case BROWSE_METHOD_USE_PREFIX:
      _display->print(F("Prefix"));
      break;

    case BROWSE_METHOD_NOT_BROWSABLE:
      _display->print(F("Fixed"));
      break;
  }   
}

void InfoScreen::shortUserPress()
{
  changeScreen(SCREEN_MAIN, SCREEN_ID_NO_PREVIOUS);
}

void InfoScreen::rotaryChange(int direction)
{
  DeviceMap &map = g_devices[_scsiId];
  if (direction == 1)
  {
    if (map.NavObjectType == NAV_OBJECT_CUE)
    {
      changeScreen(SCREEN_INFO_PAGE4, _scsiId); 
    }
    else
    {
      changeScreen(SCREEN_INFO_PAGE2, _scsiId); 
    }
  }
  else if (direction == -1)
  {
    if (map.NavObjectType == NAV_OBJECT_CUE)
    {
      changeScreen(SCREEN_INFO_PAGE2, _scsiId);
    }
    else
    {
      changeScreen(SCREEN_INFO_PAGE3, _scsiId);
    }
  }
}

#endif