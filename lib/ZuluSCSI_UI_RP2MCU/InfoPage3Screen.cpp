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

#include "InfoPage3Screen.h"
#include "ZuluSCSI_log.h"

void InfoPage3Screen::init(int index)
{
  Screen::init(index);

  _scsiId = index;
}

void InfoPage3Screen::draw()
{ 
  _display->setCursor(0,0);             
  _display->print(F("Info (3/3)"));
  
  _iconX = _display->width();

  DeviceMap &map = g_devices[_scsiId];

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


  _display->setCursor(0,22);             
  _display->print(F("LBA:"));
  _display->setCursor(52,22);       
  _display->print(map.LBA);    

  _display->setCursor(0,32);             
  _display->print(F("Bt/Sec:"));
  _display->setCursor(52,32);       
  _display->print(map.BytesPerSector);    

  _display->setCursor(0,42);             
  _display->print(F("Sc/Trk:"));
  _display->setCursor(52,42);       
  _display->print(map.SectorsPerTrack);    

  _display->setCursor(0,52);             
  _display->print(F("Hd/Cyl:"));
  _display->setCursor(52,52);       
  _display->print(map.HeadsPerCylinder);    
}

void InfoPage3Screen::shortUserPress()
{
  changeScreen(SCREEN_MAIN, SCREEN_ID_NO_PREVIOUS);
}

void InfoPage3Screen::rotaryChange(int direction)
{
  if (direction == -1)
  {
    changeScreen(SCREEN_INFO_PAGE2, _scsiId);
  }
  else if (direction == 1)
  {
    changeScreen(SCREEN_INFO, _scsiId);
  }
}

#endif