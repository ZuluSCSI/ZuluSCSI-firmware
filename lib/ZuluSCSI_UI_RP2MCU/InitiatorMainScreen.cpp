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

#include "InitiatorMainScreen.h"
#include "ZuluSCSI_log.h"

void InitiatorMainScreen::init(int index)
{
  Screen::init(index);

  _scsiId = index;

  // Init the scroller

  // Set the scroller text
}

void InitiatorMainScreen::draw()
{ 
  _display->setCursor(0,0);             
  _display->print(F("Initiator Scanning..."));
  _display->drawLine(0,10,127,10, 1);

  int yOffset = 13;
  int xOffset = 0;

  int i;
  for (i=0;i<8;i++)
  {
    if (i == 4)
    {
      xOffset = 64;
      yOffset = 12;
    }

    drawSCSIItem(xOffset, yOffset, i);

    yOffset+=13;
  }

  return;

  _display->setCursor(0,0);             
  _display->print(F("Scanning..."));
  _display->drawLine(0,10,127,10, 1);
}


void InitiatorMainScreen::tick()
{
  if (g_initiatorMessageToProcess)
  {
    g_initiatorMessageToProcess = false;
    forceDraw();
  }
  
  Screen::tick();

#ifdef SCREEN_SHOTS
    saveScreenShot();
#endif 

}

void InitiatorMainScreen::drawSCSIItem(int x, int y, int index)
{
  DeviceMap *map = &g_devices[index];

  _display->setCursor(x+10, y+1);             
  _display->print((int)index); 

  bool showType = false;

  switch(map->InitiatorDriveStatus)
  {
    case INITIATOR_DRIVE_UNKNOWN:
      _display->drawBitmap(x+25, y, icon_question, 12,12, WHITE);
      break;

    case INITIATOR_DRIVE_PROBING:
      _display->drawBitmap(x+25, y, icon_scanning, 12,12, WHITE);
      break;

    case INITIATOR_DRIVE_SCANNED:
      _display->drawBitmap(x+25, y, icon_ledoff, 12,12, WHITE);
      break;


    case INITIATOR_DRIVE_CLONABLE:
      _display->drawBitmap(x+25, y, icon_ledsemi, 12,12, WHITE);
      showType = true;
      break;
    
    case INITIATOR_DRIVE_CLONED:
      _display->drawBitmap(x+25, y, icon_ledon, 12,12, WHITE);
      showType = true;
      break;

    case INITIATOR_DRIVE_HOST:
      _display->drawBitmap(x+25, y, icon_host, 12,12, WHITE);
      break;
  }

  if (showType)
  {
    const uint8_t *deviceIcon = getIconForType(map->DeviceType, true);
    _display->drawBitmap(x+43, y, deviceIcon, 12,12, WHITE);
  }
}

#endif