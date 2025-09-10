#if defined(CONTROL_BOARD)

#ifndef SCREENSAVER_H
#define SCREENSAVER_H

#define ENABLE_LIGHTSPEED

typedef enum
{
    SCREENSAVER_RANDOM,
    SCREENSAVER_BLANK,
	SCREENSAVER_RANDOM_LOGO,
    SCREENSAVER_FLOATING_LOGO,
    SCREENSAVER_HORIZONTAL_SCROLL,
    SCREENSAVER_VERTICAL_SCROLL
#ifdef ENABLE_LIGHTSPEED
    ,SCREENSAVER_LIGHTSPEED
#endif
} SCREEN_SAVER_TYPE;

extern bool g_userInputDetected;
extern bool g_screenSaverActive;

extern void exitScreenSaver();
extern void enterScreenSaver();
extern void drawScreenSaver();
extern void screenSaverLoop();

#endif
#endif