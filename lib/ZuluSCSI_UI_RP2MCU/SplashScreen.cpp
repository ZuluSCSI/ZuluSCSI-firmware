#if defined(CONTROL_BOARD)

#include "SplashScreen.h"
#include "SystemMode.h"
#include "ZuluSCSI_log.h"

#include "control_global.h"

void SplashScreen::shortUserPress()
{
  changeScreen(SCREEN_MAIN, -1);
}

void SplashScreen::draw()
{
  _display->setTextSize(1);             
  _display->setTextColor(SSD1306_WHITE);        
  _display->drawBitmap(6,0, icon_zuluscsi, 115,56, WHITE);
  printCenteredText(_bannerText, 56);
}

void SplashScreen::setBannerText(const char *text)
{
  strcpy(_bannerText, text);
}
void SplashScreen::init(int index)
{
  Screen::init(index);

  _display->clearDisplay();

  draw();
  
  _display->display();

#ifdef SCREEN_SHOTS
    saveScreenShot();
#endif 
}

#endif