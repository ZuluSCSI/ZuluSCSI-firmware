#if defined(CONTROL_BOARD) && !defined(ENABLE_AUDIO_OUTPUT_SPDIF)

#include "NOSDScreen.h"
#include "ZuluSCSI_log.h"

void NoSDScreen::draw()
{  
  _display.setCursor(0,0);             
  _display.print(F("ZuluSCSI Control"));
  _display.drawLine(0,10,127,10, 1);
  _display.setCursor(0,16);             
  _display.print(F("NO SD CARD..."));
}

#endif