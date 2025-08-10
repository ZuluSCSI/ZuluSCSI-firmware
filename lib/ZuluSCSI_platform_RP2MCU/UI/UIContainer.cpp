#if defined(CONTROL_BOARD) && !defined(ENABLE_AUDIO_OUTPUT_SPDIF)

#include "ZuluSCSI_log.h"

#include "UI/MainScreen.h"
#include "UI/SplashScreen.h"
#include "UI/NoSDScreen.h"
#include "UI/BrowseTypeScreen.h"
#include "UI/BrowseScreen.h"
#include "UI/BrowseScreenFlat.h"
#include "UI/InfoScreen.h"
#include "UI/MessageBox.h"
#include "UI/AboutScreen.h"
#include "control.h"

extern Adafruit_SSD1306 g_display;
extern Screen *g_activeScreen;
extern int g_prevousIndex;

SplashScreen _splashScreen(g_display);
MainScreen _mainScreen(g_display);
NoSDScreen _noSDScreen(g_display);
InfoScreen _infoScreen(g_display);
BrowseScreen _browseScreen(g_display);
BrowseTypeScreen _browseTypeScreen(g_display);
BrowseScreenFlat _browseScreenFlat(g_display);
MessageBox _messageBox(g_display);
AboutScreen _aboutScreen(g_display);

// Call new card method on all screens
void sendSDCardStateChangedToScreens(bool cardIsPresent)
{
    _splashScreen.sdCardStateChange(cardIsPresent);
    _mainScreen.sdCardStateChange(cardIsPresent);
    _noSDScreen.sdCardStateChange(cardIsPresent);
    _infoScreen.sdCardStateChange(cardIsPresent);
    _browseScreen.sdCardStateChange(cardIsPresent);
    _browseTypeScreen.sdCardStateChange(cardIsPresent);
    _browseScreenFlat.sdCardStateChange(cardIsPresent);
    _messageBox.sdCardStateChange(cardIsPresent);
    _aboutScreen.sdCardStateChange(cardIsPresent);
}

// Return a pointer to a screen
Screen *GetScreen(SCREEN_TYPE type)
{
    switch(type)
    {
        case SCREEN_SPLASH:
           return &_splashScreen;
            
        case SCREEN_MAIN:
            return &_mainScreen;
            
        case SCREEN_NOSD:
            return &_noSDScreen;
            
        case SCREEN_INFO:
            return &_infoScreen;
            
        case SCREEN_BROWSE_TYPE:
            return &_browseTypeScreen;

        case SCREEN_BROWSE:
            return &_browseScreen;
            
        case SCREEN_BROWSE_FLAT:
            return &_browseScreenFlat;
            
        case MESSAGE_BOX:
            return &_messageBox;
            
        case SCREEN_ABOUT:
            return &_aboutScreen;
    }
    return NULL;
}

void changeScreen(SCREEN_TYPE type, int index)
{
    if (type == NULL)
    {
        g_activeScreen = NULL;
        return;
    }

    // Make the new screen active
    g_activeScreen = GetScreen(type);

    // If no index, set it to the previos one. This is used to restore state to the previous sceen.
    // e.g. going back to main with restore the selected SCSI id item in the main view
    if (index == -1)
    {
        index = g_prevousIndex;
    }

    // Init the Screen
    g_activeScreen->init(index);

    // Store the index, incase the next screen change index is a -1
    g_prevousIndex = index;
}

#endif