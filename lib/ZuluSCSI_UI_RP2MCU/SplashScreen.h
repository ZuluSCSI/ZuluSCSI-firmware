#if defined(CONTROL_BOARD)

#ifndef SPLASHSCREEN_H
#define SPLASHSCREEN_H

#include "Screen.h"
#include "SystemMode.h"

class SplashScreen : public Screen
{
public:
    SplashScreen(Adafruit_SSD1306 *display) : Screen(display) {}

    SCREEN_TYPE screenType() { return SCREEN_SPLASH; }

    void draw();
    void init(int index);
    void showMode(SYSTEM_MODE mode);

    void shortUserPress();

    void setBannerText(const char *text);
private:
    char _bannerText[32];
};

extern SplashScreen *_splashScreen;

#endif

#endif