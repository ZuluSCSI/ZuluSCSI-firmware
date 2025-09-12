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
#include "SettingsScreen.h"
#include "MessageBox.h"
#include "SplashScreen.h"
#include "ZuluSCSI_log.h"



extern bool g_sdAvailable;
extern bool g_rebooting;
extern Screen *g_activeScreen;
extern bool g_log_to_sd;

#define MAX_LINES 5
#define TOTAL_SETTINGS 6

void SettingsScreen::init(int index)
{
  Screen::init(index);

  // Find first active scsi ID
  _selectedIndex = index;
  if (_selectedIndex == -1)
  {
    _selectedIndex = 0;
  }

  _cursorPos = _selectedIndex;
  _screenOffset = 0;
  int diff = MAX_LINES - (_selectedIndex+1);
  if (diff < 0)
  {
    _cursorPos  += diff;
    _screenOffset -= diff;
  }
}

void SettingsScreen::drawItem(int x, int y, int index)
{
  _display->setCursor(x+10,y);  
  switch(index)
  {
    case 0:
      _display->print(F("About"));
      break;

    case 1:
      _display->print(F("Debug log: "));
      _display->print(g_log_debug ? "On" : "Off");
      break;

    case 2:
      _display->print(F("SD log: "));  
      _display->print(g_log_to_sd  ? "On" : "Off");
      break;

    case 3:
      _display->print(F("Reboot"));
      break;

    case 4:
      _display->print(F("Mass SD Mode"));
      break;

    case 5:
      _display->print(F("Mass Image Mode"));
      break;
  }

  if (_selectedIndex == index)
  {
    _display->drawBitmap(x, y, icon_select, 8,8, WHITE);
  }
}


void SettingsScreen::draw()
{
  _display->setCursor(0,0);             
  _display->print(F("Settings"));

  _display->drawLine(0,10,112,10, 1);

  if (g_sdAvailable)
  {
    _display->drawBitmap(115, 0, icon_sd, 12,12, WHITE);
  }
  else
  {
    _display->drawBitmap(115, 0, icon_nosd, 12,12, WHITE);
  }

  
  int yOffset = 14;
  int xOffset = 0;

  int i;
  for (i=0;i<MAX_LINES;i++)
  {
    int ind = i + _screenOffset;
    if (ind < TOTAL_SETTINGS)
    {
      drawItem(xOffset, yOffset, ind);
      yOffset+=10;
    }
  }
}

void SettingsScreen::sdCardStateChange(bool cardIsPresent)
{
}

void SettingsScreen::action()
{
  volatile uint32_t* scratch0 = (uint32_t *)(WATCHDOG_BASE + WATCHDOG_SCRATCH0_OFFSET);

  switch(_selectedIndex)
  {
    case 0:
    {
      _splashScreen->setBannerText(g_log_short_firmwareversion);
      changeScreen(SCREEN_SPLASH, _selectedIndex);
      break;
    }

    case 1:
    {
      *scratch0 = 0;
      g_log_debug = !g_log_debug;
      forceDraw();
      break;
    }

    case 2:
    {
      *scratch0 = 0;
      g_log_to_sd  = !g_log_to_sd ;
      forceDraw();
      break;
    }

    case 3:
    {
      *scratch0 = 0;
      g_rebooting = true;

      _messageBox->setBlockingMode(true);
      _messageBox->setText("-- Info --", "Rebooting...", "");
      changeScreen(MESSAGE_BOX, -1);
      g_activeScreen->tick();
      break;
    }

    case 4:
    {
      *scratch0 = REBOOT_INTO_MASS_STORAGE_MAGIC_NUM;
      g_rebooting = true;

      _messageBox->setBlockingMode(true);
      _messageBox->setText("-- Info --", "Rebooting into", "MSC SD...");
      changeScreen(MESSAGE_BOX, -1);
      g_activeScreen->tick();
      break;
    }

    case 5:
    {
     *scratch0 = REBOOT_INTO_MASS_STORAGE_IMAGES_MAGIC_NUM;
      g_rebooting = true;

      _messageBox->setBlockingMode(true);
      _messageBox->setText("-- Info --", "Rebooting into", "MSC Image...");
      changeScreen(MESSAGE_BOX, -1);
      g_activeScreen->tick();
      break;
    }
  }
}
void SettingsScreen::shortRotaryPress()
{
  action();
}

void SettingsScreen::shortEjectPress()
{
  action();
}

void SettingsScreen::shortUserPress()
{
  changeScreen(SCREEN_MAIN, -1);
}

void SettingsScreen::rotaryChange(int direction)
{
  int totalLines = TOTAL_SETTINGS;

  if (direction == 1)
  {
    _selectedIndex++;
    if (_selectedIndex == totalLines)
    {
      _selectedIndex--;
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
    _selectedIndex--;
    if (_selectedIndex == -1)
    {
      _selectedIndex++;
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