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

#include "ui.h"
#include "MainScreen.h"
#include "MessageBox.h"
#include "SplashScreen.h"
#include "ZuluSCSI_log.h"

#include "cache.h"

#define DEVICES_PER_PAGE 8
#define DEVICES_PER_COLUMN 4
extern bool g_sdAvailable;

void MainScreen::init(int index)
{
  Screen::init(index);

  // Find first active scsi ID
  if (index == -1 || _selectedDevice == -1) // TODO just a test: _selectedDevice == -1? does it do anything when StateChange() called?
  {
    _selectedDevice = -1;
    int i;
    for (i=0;i<8;i++)
    {
      if (g_devices[i].Active)
      {
        _selectedDevice = i;
        break;
      }
    }
  }
  else
  {
    _selectedDevice = index;
  }

  if (_selectedDevice != -1)
  {
    _deviceMap = &g_devices[_selectedDevice];
  }
  else
  {
    _deviceMap = NULL;
  }
}

void MainScreen::draw()
{
  _display->setCursor(0,0);             
  _display->print(F("SCSI Map"));

  int page = 0;
  int deviceOffset = 0;
  if (_selectedDevice > -1)
  {
    page = (_selectedDevice/DEVICES_PER_PAGE);
    deviceOffset = page * DEVICES_PER_PAGE;
  }
  
  int totalPages = S2S_MAX_TARGETS / DEVICES_PER_PAGE;
  if (totalPages > 1)
  {
    _display->print(" (");
    _display->print((page+1));
    _display->print("/");
    _display->print(totalPages);
    _display->print(")");
  }

  _display->drawLine(0,10,112,10, 1);

  if (g_sdAvailable)
  {
    _display->drawBitmap(115, 0, icon_sd, 12,12, WHITE);
  }
  else
  {
    _display->drawBitmap(115, 0, icon_nosd, 12,12, WHITE);
  }

  int xOffset = 0;
  int yOffset = 13;
  int i;
  for (i=0;i<DEVICES_PER_PAGE;i++)
  {
    if (i == DEVICES_PER_COLUMN)
    {
      xOffset = 64;
      yOffset = 13;
    }

    drawSCSIItem(xOffset, yOffset, i + deviceOffset);

    yOffset+=13;
  }
}

void MainScreen::tick()
{
  Screen::tick();
}

void MainScreen::sdCardStateChange(bool cardIsPresent)
{
  _selectedDevice = -1; // reset it, so it's recalced
  _deviceMap = NULL;
}

void MainScreen::shortRotaryPress()
{
  if (_selectedDevice == -1)
  {
    return;
  }

  changeScreen(SCREEN_INFO, _selectedDevice);
}

void MainScreen::shortUserPress()
{
  
}

void MainScreen::shortEjectPress()
{
  if (_selectedDevice == -1 || _deviceMap == NULL)
  {
    return;
  }

  switch(_deviceMap->BrowseMethod)
  {
    case BROWSE_METHOD_IMDDIR:
      if (_deviceMap->BrowseScreenType == 0)
      {
          changeScreen(SCREEN_BROWSE, _selectedDevice);
      }
      else
      {
          changeScreen(SCREEN_BROWSE_FLAT, _selectedDevice);
      }
      break;
    case BROWSE_METHOD_IMGX:
    case BROWSE_METHOD_USE_PREFIX:
      changeScreen(SCREEN_BROWSE_FLAT, _selectedDevice);
      break;

    
    case BROWSE_METHOD_NOT_BROWSABLE:
      _messageBox->setReturnScreen(SCREEN_MAIN);
      _messageBox->setText("-- Warning --", "Browsing not", "Supported...");
      changeScreen(MESSAGE_BOX, _selectedDevice);
      break;
  }
}

void MainScreen::longUserPress()
{
#ifdef SCREEN_SHOTS
  Screen::longUserPress(); //TODO just for screensjjot
#endif

  changeScreen(SCREEN_SETTINGS, -1);
}

void MainScreen::longEjectPress()
{
  if (_selectedDevice == -1 || _deviceMap == NULL)
  {
    return;
  }

  switch(_deviceMap->BrowseMethod)
  {
    case BROWSE_METHOD_IMDDIR:
      if ((doesDeviceHaveAnyCategoryFiles(_selectedDevice) == 0 && !_deviceMap->HasDirs) && g_cacheActive)
      {
        _messageBox->setReturnScreen(SCREEN_MAIN);
        _messageBox->setText("-- Warning --", "No folders or", "categories...");
        changeScreen(MESSAGE_BOX, _selectedDevice);
      }
      else
      {
        changeScreen(SCREEN_BROWSE_TYPE, _selectedDevice); 
      }
      break;

    case BROWSE_METHOD_IMGX:
      _messageBox->setReturnScreen(SCREEN_MAIN);
      _messageBox->setText("-- Warning --", "No options", "for IMGx style");
      changeScreen(MESSAGE_BOX, _selectedDevice);
      break;

    case BROWSE_METHOD_USE_PREFIX:
      _messageBox->setReturnScreen(SCREEN_MAIN);
      _messageBox->setText("-- Warning --", "No options", "for prefix style");
      changeScreen(MESSAGE_BOX, _selectedDevice);
      break;

    case BROWSE_METHOD_NOT_BROWSABLE:
      _messageBox->setReturnScreen(SCREEN_MAIN);
      _messageBox->setText("-- Warning --", "Browsing not", "Supported...");
      changeScreen(MESSAGE_BOX, _selectedDevice);
      break;
  }
}

void MainScreen::rotaryChange(int direction)
{
  if (_selectedDevice == -1) // there aren't any, so just return (it would have been set to something other than -1 if there were)
  {
    return;
  }
  
  int i;
  bool found = false;

  if (direction == 1)
  {
    // Find next valid scsi id
    for (i=_selectedDevice+1; i<8;i++)    
    {
      if (g_devices[i].Active)
      {
        _selectedDevice = i;
        found = true;
        break;
      }
    }
    if (!found)
    {
      for (i=0; i<_selectedDevice;i++)    
      {
        if (g_devices[i].Active)
        {
          _selectedDevice = i;
          found = true;
          break;
        }
      }
    }
  }
  else // dir -1
  {
    // Find next valid scsi id
    for (i=_selectedDevice-1; i>=0;i--)    
    {
      if (g_devices[i].Active)
      {
        _selectedDevice = i;
        found = true;
        break;
      }
    }
    if (!found)
    {
      for (i=7; i>=_selectedDevice;i--)    
      {
        if (g_devices[i].Active)
        {
          _selectedDevice = i;
          found = true;
          break;
        }
      }
    }
  }

  if (found)
  {
    _deviceMap = &g_devices[_selectedDevice];
    forceDraw();
  }
}

void MainScreen::drawSCSIItem(int x, int y, int index)
{
  DeviceMap *map = &g_devices[index];

  _display->setCursor(x+10, y+2);             
  _display->print((int)index); 

  if (_selectedDevice == index)
  {
    _display->drawBitmap(x, y+1, icon_select, 8,8, WHITE);
  }

  if (map->Active)
  {
    const uint8_t *deviceIcon = getIconForType(map->DeviceType, true);
    _display->drawBitmap(x+36, y, deviceIcon, 12,12, WHITE);

    if (map->IsRom)
    {
      _display->drawBitmap(x+51, y, icon_rom, 12,12, WHITE);  
    }
    else if (map->IsRaw)
    {
      _display->drawBitmap(x+51, y, icon_raw, 12,12, WHITE);  
    }

    _display->drawBitmap(x+22, y, icon_ledon, 12,12, WHITE);
    
  }
  else
  {
    _display->drawBitmap(x+22, y, icon_ledoff, 12,12, WHITE);
  }
}

#endif