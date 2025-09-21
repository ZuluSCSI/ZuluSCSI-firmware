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
#include "MessageBox.h"
#include "ZuluSCSI_log.h"
#include "ZuluSCSI_disk.h"

#include "control_global.h"

extern Screen *g_activeScreen;
extern bool g_led_blinking;

void MessageBox::init(int index)
{
  Screen::init(index);

  _index = index;

  _isActive = true;
}

void MessageBox::draw()
{
    Screen::draw();

    // NB: No Clear, as it should be drawn over the top of the current display     

    Rectangle bounds = Rectangle{{15,10}, {98, 44}};

    _display->fillRect(bounds.topLeft.x,   bounds.topLeft.y,   bounds.size.width,  bounds.size.height, 0);
    _display->drawRoundRect(bounds.topLeft.x+2, bounds.topLeft.y+2, bounds.size.width-4, bounds.size.height-4, 3, 1);
    _display->drawRoundRect(bounds.topLeft.x+3, bounds.topLeft.y+3, bounds.size.width-6, bounds.size.height-6, 3, 1);

    drawText(bounds, bounds.topLeft.y + 6, _title);
    _display->drawLine(bounds.topLeft.x+2, bounds.topLeft.y + 15, bounds.topLeft.x+bounds.size.width-4, bounds.topLeft.y + 15, 1);
    drawText(bounds, bounds.topLeft.y + 19, _line1);
    drawText(bounds, bounds.topLeft.y + 28, _line2);

#ifdef SCREEN_SHOTS
    saveScreenShot();
#endif 
}

void MessageBox::tick()
{
  Screen::tick();

  if (_conditionPendingLoadComplete)
  {
    if (g_pendingLoadComplete > -1)
    {
        // Now wait for the flashing to stop
        if (!g_led_blinking)
        {
          _conditionPendingLoadComplete = false;
          changeScreen(_return, _index);
        }
    }
  }
}

void MessageBox::setBlockingMode(bool blocking)
{
  _blocking = blocking;
}
void MessageBox::shortRotaryPress()
{
  if (_blocking)
  {
      return;
  }
  changeScreen(_return, _index);
  _isActive = false;
}

void MessageBox::shortUserPress()
{
  if (_blocking)
  {
      return;
  }
  changeScreen(_return, _index);
  _isActive = false;
}

void MessageBox::shortEjectPress()
{
  if (_blocking)
  {
      return;
  }
  changeScreen(_return, _index);
  _isActive = false;
}

void MessageBox::rotaryChange(int direction)
{
  if (_blocking)
  {
      return;
  }
  changeScreen(_return, _index);
  _isActive = false;
}

void MessageBox::setReturnScreen(SCREEN_TYPE ret)
{
    _return = ret;
}

void MessageBox::setReturnConditionPendingLoadComplete()
{
    _conditionPendingLoadComplete = true;
}

void MessageBox::setText(const char *title, const char *line1, const char *line2)
{
    _title = title;
    _line1 = line1;
    _line2 = line2;
}

bool MessageBox::clearScreenOnDraw()
{
  return false;
}

void MessageBox::ShowModal(int index, const char *title, const char *line1, const char *line2)
{
  return;
  
  if (g_activeScreen != NULL)
  {
    _return = g_activeScreen->screenType();
  }
  else
  {
    _return = (SCREEN_TYPE)SCREEN_NONE;
  }

  setText(title, line1, line2);

  changeScreen(MESSAGE_BOX, index);

  tick();

  while(_messageBox->_isActive)
  {
      controlLoop();
  }
}

void MessageBox::drawText(Rectangle bound, int yOffset, const char *msg)
{
    // std::string toDisplay = std::string(msg);
    int16_t x = 0, y = 0;
    Size toDispSize;
    _display->getTextBounds(msg, 0 ,0, &x, &y, &toDispSize.width, &toDispSize.height);

    auto dispBox = bound.MakeCentered(toDispSize);
    _display->setCursor(dispBox.topLeft.x, yOffset);
    _display->print(msg);
}

#endif