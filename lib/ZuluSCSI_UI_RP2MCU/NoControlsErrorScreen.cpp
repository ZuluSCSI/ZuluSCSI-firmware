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
#include "NoControlsErrorScreen.h"
#include "SplashScreen.h"
#include "ZuluSCSI_log.h"

#include "cache.h"

extern bool g_sdAvailable;

void NoControlsErrorScreen::init(int index)
{
  Screen::init(index);
}

void NoControlsErrorScreen::draw()
{
  _display->setCursor(0,0);
  _display->print(F("ZuluSCSI Error"));
  _display->drawLine(0,10,112,10, 1);
  if (g_sdAvailable)
  {
    _display->drawBitmap(115, 0, icon_sd, 12,12, WHITE);
  }
  else
  {
    _display->drawBitmap(115, 0, icon_nosd, 12,12, WHITE);
  }
  _display->setCursor(0,16);
  _display->print(F("Failed to Init I2C"));
  _display->setCursor(0,28);
  _display->print(F("GPIO expander for"));
  _display->setCursor(0,38);
  _display->print(F("navigation controls"));

  printCenteredText(g_log_short_firmwareversion, 50);
}

void NoControlsErrorScreen::tick()
{
  Screen::tick();
}

#endif