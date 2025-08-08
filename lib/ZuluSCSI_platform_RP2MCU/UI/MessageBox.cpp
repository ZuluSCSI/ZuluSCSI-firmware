#if defined(CONTROL_BOARD) && !defined(ENABLE_AUDIO_OUTPUT_SPDIF)

#include "MessageBox.h"
#include "ZuluSCSI_log.h"
#include "ZuluSCSI_disk.h"

#include "control_global.h"

void MessageBox::init(int index)
{
  Screen::init(index);

  _index = index;
}

void MessageBox::draw()
{
    Screen::draw();

    // NB: No Clear, as it should be drawn over the top of the current display     

    Rectangle bounds = Rectangle{{15,10}, {98, 44}};

    _display.fillRect(bounds.topLeft.x,   bounds.topLeft.y,   bounds.size.width,  bounds.size.height, 0);
    _display.drawRect(bounds.topLeft.x+2, bounds.topLeft.y+2, bounds.size.width-4, bounds.size.height-4, 1);
    _display.drawRect(bounds.topLeft.x+3, bounds.topLeft.y+3, bounds.size.width-6, bounds.size.height-6, 1);

    drawText(bounds, bounds.topLeft.y + 6, _title);
    _display.drawLine(bounds.topLeft.x+2, bounds.topLeft.y + 15, bounds.topLeft.x+bounds.size.width-4, bounds.topLeft.y + 15, 1);
    drawText(bounds, bounds.topLeft.y + 19, _line1);
    drawText(bounds, bounds.topLeft.y + 28, _line2);
}

void MessageBox::tick()
{
  Screen::tick();

  if (_conditionPendingLoadComplete)
  {
    if (g_pendingLoadComplete > -1)
    {
        _conditionPendingLoadComplete = false;
        changeScreen(_return, _index);
    }
  }
}

void MessageBox::shortRotaryPress()
{
    changeScreen(_return, _index);
}

void MessageBox::shortUserPress()
{
    changeScreen(_return, _index);
}

void MessageBox::shortEjectPress()
{
    changeScreen(_return, _index);
}

void MessageBox::rotaryChange(int direction)
{
  changeScreen(_return, _index);
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

void MessageBox::drawText(Rectangle bound, int yOffset, const char *msg)
{
    // std::string toDisplay = std::string(msg);
    int16_t x = 0, y = 0;
    Size toDispSize;
    _display.getTextBounds(msg, 0 ,0, &x, &y, &toDispSize.width, &toDispSize.height);

    auto dispBox = bound.MakeCentered(toDispSize);
    _display.setCursor(dispBox.topLeft.x, yOffset);
    _display.print(msg);
}

#endif