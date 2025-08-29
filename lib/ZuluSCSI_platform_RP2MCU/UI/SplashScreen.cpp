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
  _display.setCursor(0,56);             

  _display.setTextSize(1);    

  switch(mode)
  {
    case SYSTEM_NORMAL:
      _display.print("Normal Mode");
      break;

    case SYSTEM_INITIATOR:
      _display.print("Initiator Mode");
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
  _display.setCursor(0,56);             
  _display.print(g_log_short_firmwareversion);

  _display.display();

  

// delay(1500);
}

#endif