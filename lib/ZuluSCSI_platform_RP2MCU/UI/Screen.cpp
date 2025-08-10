#if defined(CONTROL_BOARD) && !defined(ENABLE_AUDIO_OUTPUT_SPDIF)

#include "screen.h"
#include "ZuluSCSI_log.h"

SCREEN_TYPE Screen::screenType()
{
  return (SCREEN_TYPE)SCREEN_NONE;
}

void Screen::init(int index)
{
    _nextRefresh = make_timeout_time_ms(360);
    _hasDrawn = false;
    _halted = false;
}

void Screen::draw()
{
}

void Screen::tick()
{
    // If if it's never been drawn draw it (this is mainly for screens with no scroller, to do the initial draw)
    if (_hasDrawn)
    {
      // If there are no scrollers or it's halted then do nothing
      if (_totalScrollers == 0 || _halted)
      {
          return;
      }

      // If not enough time has elapsed for a refresh, do nothing
      int64_t del = absolute_time_diff_us (get_absolute_time(), _nextRefresh);
      if (del > 0) 
      {
          return;
      }
    }
    
    updateScrollers(get_absolute_time());
  
    if (clearScreenOnDraw())
    {
      _display.clearDisplay();
    }

    // defaults for text
    _display.setTextSize(1);             
    _display.setTextColor(SSD1306_WHITE);    

    displayScrollers();

    draw();

    _display.display();

    _hasDrawn = true;
}

void Screen::haltUIUpdates()
{
  _halted = true;
}

void Screen::sdCardStateChange(bool cardIsPresent)
{
}

void Screen::forceDraw()
{
  _hasDrawn = false;
}

void Screen::shortRotaryPress()
{
}

void Screen::shortUserPress()
{
}

void Screen::shortEjectPress()
{
}

void Screen::longRotaryPress()
{
}

void Screen::longUserPress()
{
}

void Screen::longEjectPress()
{
}

void Screen::rotaryChange(int direction)
{
}

bool  Screen::clearScreenOnDraw()
{
  return true;
}

const uint8_t *Screen::getIconForType(S2S_CFG_TYPE deviceType, bool loaded)
{
  switch (deviceType)
  {
    case S2S_CFG_OPTICAL:
      return icon_cd;

    case S2S_CFG_FIXED:
      return icon_fixed;

    case S2S_CFG_MO:
      return icon_mo;
    
    case S2S_CFG_FLOPPY_14MB:
      return icon_floppy;
    
    case S2S_CFG_ZIP100:
      return icon_zip;
    
    case S2S_CFG_REMOVABLE:
      return icon_removable;
    
    case S2S_CFG_SEQUENTIAL:
      return icon_sequential2;
    
    case S2S_CFG_NETWORK:
      return icon_network;

    case S2S_CFG_NOT_SET:
    default:
      break;
  }
  return  icon_question;
}

void Screen::setScrollerText(int index, const char *text)
{
    _scrollingText[index].SetToDisplay(text);
}

void Screen::initScrollers(int total)
{
    _totalScrollers = total;

    int i;
    for (i=0;i<_totalScrollers;i++)
    {
        _scrollingText[i].graph = &_display;
    }
}

void Screen::setupScroller(int index, uint16_t x, uint16_t y, uint16_t w, uint16_t h, int font)
{
    Rectangle rect = Rectangle{{x,y}, {w, h}};

    _scrollingText[index].bounds = rect;
    _scrollingText[index]._font = font;
}

void Screen::resetScrollerDelay()
{
  int i;
    for (i=0;i<_totalScrollers;i++)
    {
        _scrollingText[i].ResetDelay();
    }
}

void Screen::drawIconFromRight(const uint8_t *icon, int extraSpace, int y)
{
    _display.drawBitmap(_iconX, y, icon, 12,12, WHITE);
    _iconX -= 14;
    _iconX -= extraSpace;
}

void Screen::makeImageSizeStr(uint64_t size, char *buffer) 
{
  if (size > 1073741824) 
  {
    dtostrf((size / 1073741824.0f), 5, 1, buffer);
    strcat(buffer, "G");

  } 
  else if (size > 1048576) 
  {
    dtostrf((size / 1048576.0f), 5, 1, buffer);
    strcat(buffer, "M");
  } 
  else if (size > 1024) 
  {
    dtostrf((size / 1024.0f), 5, 1, buffer);
    strcat(buffer, "k");
  } 
  else 
  {
    dtostrf(size, 5, 1, buffer);
  }
}

void Screen::updateScrollers(absolute_time_t now)
{
    int i;
    for (i=0;i<_totalScrollers;i++)
    {
        _scrollingText[i].CheckAndUpdateScrolling(now);
    }

    _nextRefresh = make_timeout_time_ms(360);
}

void Screen::displayScrollers()
{
    int i;
    for (i=0;i<_totalScrollers;i++)
    {
        _scrollingText[i].Display();
    }
}

#endif