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


#define DEVICES_PER_PAGE 8
#define DEVICES_PER_COLUMN 4
extern bool g_sdAvailable;
extern bool g_rebooting;
extern Screen *g_activeScreen;

void SettingsScreen::init(int index)
{
  Screen::init(index);

  // Find first active scsi ID
  _selectedIndex = 0;
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

  _display->setCursor(16,22);             
  _display->print(F("Reboot"));

  _display->setCursor(16,32);             
  _display->print(F("Mass Storage Mode"));

  _display->setCursor(16,42);             
  _display->print(F("About"));

  int yOffset = 22 + (_selectedIndex * 10);

  _display->drawBitmap(0, yOffset, icon_select, 8,8, WHITE);
}

void SettingsScreen::sdCardStateChange(bool cardIsPresent)
{
}


void SettingsScreen::action()
{
  switch(_selectedIndex)
  {
    case 0:
    {
      g_rebooting = true;
      _messageBox->setBlockingMode(true);
      _messageBox->setText("-- Info --", "Rebooting...", "");
      changeScreen(MESSAGE_BOX, -1);
      g_activeScreen->tick();
      break;
    }

    case 1:
    {
      volatile uint32_t* scratch0 = (uint32_t *)(WATCHDOG_BASE + WATCHDOG_SCRATCH0_OFFSET);
      *scratch0 = REBOOT_INTO_MASS_STORAGE_MAGIC_NUM;
      g_rebooting = true;

      _messageBox->setBlockingMode(true);
      _messageBox->setText("-- Info --", "Rebooting into", "MSC Mode...");
      changeScreen(MESSAGE_BOX, -1);
      g_activeScreen->tick();
      break;
    }

    case 2:
    {
      _splashScreen->setBannerText(g_log_short_firmwareversion);
      changeScreen(SCREEN_SPLASH, -1);
      break;
    }
  }
}
void SettingsScreen::shortRotaryPress()
{
  action();
}

void SettingsScreen::shortUserPress()
{
  changeScreen(SCREEN_MAIN, -1);
}

void SettingsScreen::shortEjectPress()
{
  action();
}

void SettingsScreen::longUserPress()
{

}

void SettingsScreen::longEjectPress()
{
  
}

void SettingsScreen::rotaryChange(int direction)
{
  if (direction == 1)
  {
    _selectedIndex++;
    if (_selectedIndex == 3)
    {
      _selectedIndex =0;
    }
  }
  else if (direction == -1)
  {
    _selectedIndex--;
    if (_selectedIndex == -1)
    {
      _selectedIndex =2;
    }
  }
  forceDraw();
}

#endif