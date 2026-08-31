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

#include "Screen.h"
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
    _init_index = index;
}

int Screen::getOriginalIndex()
{
  return _init_index;
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
      _display->clearDisplay();
    }

    // defaults for text
    _display->setTextSize(1);             
    _display->setTextColor(SSD1306_WHITE);    

    displayScrollers();

    draw();

    _display->display();

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
#ifdef SCREEN_SHOTS
    saveScreenShot();
#endif
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
      return icon_tape;
    
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
        _scrollingText[i].graph = _display;
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

void Screen::printCenteredText(const char *text, int y)
{
   int16_t lx = 0, ly = 0;
   Size toDispSize;
  
   _display->getTextBounds(text, 0 ,0, &lx, &ly, &toDispSize.width, &toDispSize.height);
    int x = 64 - (toDispSize.width/2);

  _display->setCursor(x, y);
  _display->print(text);
}

void Screen::printNumberFromTheRight(int number, int extraSpace, int y)
{
  int16_t lx = 0, ly = 0;
  Size toDispSize;

  char number_text[13];
  itoa(number, number_text, 10);
  _display->getTextBounds(number_text, 0 ,0, &lx, &ly, &toDispSize.width, &toDispSize.height);
  _iconX -= toDispSize.width;
  _display->setCursor(_iconX, y);
  _display->print(number_text);
  _iconX -= 14;
  _iconX -= extraSpace;
}

void Screen::drawIconFromRight(const uint8_t *icon, int extraSpace, int y)
{
    _display->drawBitmap(_iconX, y, icon, 12,12, WHITE);
    _iconX -= 14;
    _iconX -= extraSpace;
}

void Screen::makeTimeStr(int seconds, char *buffer)
{
  int pos = 0;
  int hours = seconds / 3600;
  int rem =  seconds - (hours * 3600);

  if (hours > 99)
  {
    hours = 99; // Hard cap
  }

  int mins = rem / 60;
  rem = rem - (mins * 60);

//  logmsg("seconds = ", seconds, "  H:", hours, " M:", mins, " S:", rem);

  //strcpy(buffer, ""); // blank to start

  if (hours < 10)
  {
    buffer[pos++] = '0';
  }
  else
  {
    int h = hours / 10; // add 1st digit of hours
    buffer[pos++] = h + '0';

    hours -= (h * 10);
  }
  
  // low hour digit
  buffer[pos++] = hours + '0';

  // add 1st :  
  buffer[pos++] = ':';
  
  if (mins < 10)
  {
    buffer[pos++] = '0';
  }
  else
  {
    int m = mins / 10;
    buffer[pos++] = m + '0';

    mins -= (m * 10);
  }

  // low minute digit
  buffer[pos++] = mins + '0';

  buffer[pos++] = ':';

  if (rem < 10)
  {
    buffer[pos++] = '0';
  }
  else
  {
    int s = rem / 10;
    buffer[pos++] = s + '0';

    rem -= (s * 10);
  }
  
  // lower sec
  buffer[pos++] = rem + '0';
  buffer[pos++] = 0;

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

FsFile Screen::createFile()
{
  int i;
  char file[64], b[64];
  for (i=0;i<1000;i++)
  {
    strcpy(file, "screen");
    itoa(i, b, 10);
    strcat(file,b);
    strcat(file,".bmp");

    logmsg("--> Filename:", file);

    if (!SD.exists(file))
    {
      break;
    }
  }

  FsVolume *vol = SD.vol();
  return vol->open(file, O_WRONLY | O_CREAT);
}

void Screen::saveScreenShot()
{
  logmsg("Creating screen shot");
  FsFile fileHandle = createFile();

  int w = 128;
  int h = 64;
  int imgSize = w*h;

  // set fileSize (used in bmp header)
  int rowSize = 4 * ((3*w + 3)/4);      // how many bytes in the row (used to create padding)
  int fileSize = 54 + h*rowSize;        // headers (54 bytes) + pixel data

  unsigned char *img = NULL;            // image data
  img = (unsigned char *)malloc(3*imgSize);

  for (int y=0; y<h; y++) {
    for (int x=0; x<w; x++) {
      bool pix = _display->getPixel(x,y);

      int colorVal = pix ? 255 : 0;                   // classic formula for px listed in line
      img[(y*w + x)*3+0] = (unsigned char)(colorVal);    // R
      img[(y*w + x)*3+1] = (unsigned char)(colorVal);    // G
      img[(y*w + x)*3+2] = (unsigned char)(colorVal);    // B
      // padding (the 4th byte) will be added later as needed...
    }
  }
  // create padding (based on the number of pixels in a row
  unsigned char bmpPad[rowSize - 3*w];
  for (int i=0; i<sizeof(bmpPad); i++) {         // fill with 0s
    bmpPad[i] = 0;
  }

  // create file headers (also taken from StackOverflow example)
  unsigned char bmpFileHeader[14] = {            // file header (always starts with BM!)
    'B','M', 0,0,0,0, 0,0, 0,0, 54,0,0,0   };
  unsigned char bmpInfoHeader[40] = {            // info about the file (size, etc)
    40,0,0,0, 0,0,0,0, 0,0,0,0, 1,0, 24,0   };

  bmpFileHeader[ 2] = (unsigned char)(fileSize      );
  bmpFileHeader[ 3] = (unsigned char)(fileSize >>  8);
  bmpFileHeader[ 4] = (unsigned char)(fileSize >> 16);
  bmpFileHeader[ 5] = (unsigned char)(fileSize >> 24);

  bmpInfoHeader[ 4] = (unsigned char)(       w      );
  bmpInfoHeader[ 5] = (unsigned char)(       w >>  8);
  bmpInfoHeader[ 6] = (unsigned char)(       w >> 16);
  bmpInfoHeader[ 7] = (unsigned char)(       w >> 24);
  bmpInfoHeader[ 8] = (unsigned char)(       h      );
  bmpInfoHeader[ 9] = (unsigned char)(       h >>  8);
  bmpInfoHeader[10] = (unsigned char)(       h >> 16);
  bmpInfoHeader[11] = (unsigned char)(       h >> 24);

  // write the file (thanks forum!)
  fileHandle.write(bmpFileHeader, sizeof(bmpFileHeader));    // write file header
  fileHandle.write(bmpInfoHeader, sizeof(bmpInfoHeader));    // " info header

  for (int i=0; i<h; i++) {                            // iterate image array
    fileHandle.write(img+(w*(h-i-1)*3), 3*w);                // write px data
    fileHandle.write(bmpPad, (4-(w*3)%4)%4);                 // and padding as needed
  }
  free(img);

  fileHandle.close();    
}

#endif