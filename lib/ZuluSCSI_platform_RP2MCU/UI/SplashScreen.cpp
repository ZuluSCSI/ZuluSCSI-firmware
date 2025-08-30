#if defined(CONTROL_BOARD) && !defined(ENABLE_AUDIO_OUTPUT_SPDIF)

#include "SplashScreen.h"
#include "SystemMode.h"
#include "ZuluSCSI_log.h"
#include "ZuluSCSI_Platform.h"

#include "control_global.h"

void SplashScreen::showMode(SYSTEM_MODE mode)
{
  _display.clearDisplay();

  _display.setTextSize(1);             
  _display.setTextColor(SSD1306_WHITE);        
  
  // _display.drawBitmap(0,0, icon_zuluscsi, 128,64, WHITE);
  _display.drawBitmap(6,0, icon_zuluscsi, 115,56, WHITE);
  
  _display.setTextSize(1);    

  switch(mode)
  {
    case SYSTEM_NORMAL:
      printCenteredText("Normal Mode", 56);
      break;

    case SYSTEM_INITIATOR:
      printCenteredText("Initiator Mode", 56);
      break;
  }

  _display.display();
  delay(1000);
}

void SplashScreen::init(int index)
{
  Screen::init(index);

  _display.clearDisplay();

  _display.setTextSize(1);             
  _display.setTextColor(SSD1306_WHITE);        
  
  // _display.drawBitmap(0,0, icon_zuluscsi, 128,64, WHITE);
  _display.drawBitmap(6,0, icon_zuluscsi, 115,56, WHITE);

  printCenteredText(g_log_short_firmwareversion, 56);
  
  _display.display();
}

#endif