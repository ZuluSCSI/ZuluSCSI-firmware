#if defined(CONTROL_BOARD)

#include "ZuluSCSI_log.h"

#include "SystemMode.h"

#include "MainScreen.h"
#include "SplashScreen.h"
#include "NoSDScreen.h"
#include "BrowseTypeScreen.h"
#include "BrowseScreen.h"
#include "BrowseScreenFlat.h"
#include "InfoScreen.h"
#include "MessageBox.h"
#include "CopyScreen.h"
#include "InitiatorMainScreen.h"
#include "control.h"
#include <ZuluSCSI_buffer_control.h>

extern Adafruit_SSD1306 *g_display;
extern Screen *g_activeScreen;
extern int g_prevousIndex;

SplashScreen *_splashScreen;
MainScreen *_mainScreen;
NoSDScreen *_noSDScreen;
InfoScreen *_infoScreen;
BrowseScreen *_browseScreen;
BrowseTypeScreen *_browseTypeScreen;
BrowseScreenFlat *_browseScreenFlat;
MessageBox *_messageBox;
CopyScreen *_copyScreen;
InitiatorMainScreen *_initiatorMainScreen;


bool initScreens()
{
    _splashScreen = new (reserve_buffer_align(sizeof(SplashScreen), 4)) SplashScreen(g_display);
    _mainScreen = new (reserve_buffer_align(sizeof(MainScreen), 4)) MainScreen(g_display);
    _noSDScreen = new (reserve_buffer_align(sizeof(NoSDScreen), 4)) NoSDScreen(g_display);
    _infoScreen = new (reserve_buffer_align(sizeof(InfoScreen), 4)) InfoScreen(g_display);
    _browseScreen = new (reserve_buffer_align(sizeof(BrowseScreen), 4)) BrowseScreen(g_display);
    _browseTypeScreen = new (reserve_buffer_align(sizeof(BrowseTypeScreen), 4)) BrowseTypeScreen(g_display);
    _browseScreenFlat = new (reserve_buffer_align(sizeof(BrowseScreenFlat), 4)) BrowseScreenFlat(g_display);
    _messageBox = new (reserve_buffer_align(sizeof(MessageBox), 4)) MessageBox(g_display);
    _copyScreen = new (reserve_buffer_align(sizeof(CopyScreen), 4)) CopyScreen(g_display);
    _initiatorMainScreen = new (reserve_buffer_align(sizeof(InitiatorMainScreen), 4)) InitiatorMainScreen(g_display);
    return true;
}

// Call new card method on all screens
void sendSDCardStateChangedToScreens(bool cardIsPresent)
{
    _splashScreen->sdCardStateChange(cardIsPresent);
    _mainScreen->sdCardStateChange(cardIsPresent);
    _noSDScreen->sdCardStateChange(cardIsPresent);
    _infoScreen->sdCardStateChange(cardIsPresent);
    _browseScreen->sdCardStateChange(cardIsPresent);
    _browseTypeScreen->sdCardStateChange(cardIsPresent);
    _browseScreenFlat->sdCardStateChange(cardIsPresent);
    _messageBox->sdCardStateChange(cardIsPresent);
    _copyScreen->sdCardStateChange(cardIsPresent);
    _initiatorMainScreen->sdCardStateChange(cardIsPresent);
}

// Return a pointer to a screen
Screen *GetScreen(SCREEN_TYPE type)
{
    switch(type)
    {
        case SCREEN_NONE:
           return NULL;

        case SCREEN_SPLASH:
           return _splashScreen;
            
        case SCREEN_MAIN:
            return _mainScreen;
            
        case SCREEN_NOSD:
            return _noSDScreen;
            
        case SCREEN_INFO:
            return _infoScreen;
            
        case SCREEN_BROWSE_TYPE:
            return _browseTypeScreen;

        case SCREEN_BROWSE:
            return _browseScreen;
            
        case SCREEN_BROWSE_FLAT:
            return _browseScreenFlat;
            
        case MESSAGE_BOX:
            return _messageBox;

        case SCREEN_COPY:
            return _copyScreen;

        case SCREEN_INITIATOR_MAIN:
            return _initiatorMainScreen;
    }
    return NULL;
}

const char *GetScreenName(SCREEN_TYPE type)
{
    switch(type)
    {
        case SCREEN_NONE:
           return "None";

        case SCREEN_SPLASH:
           return "Splash";
            
        case SCREEN_MAIN:
            return "Main";
            
        case SCREEN_NOSD:
            return "NoSD";
            
        case SCREEN_INFO:
            return "Info";
            
        case SCREEN_BROWSE_TYPE:
            return "Browse Type";

        case SCREEN_BROWSE:
            return "Browse";
            
        case SCREEN_BROWSE_FLAT:
            return "Browse Flat";
            
        case MESSAGE_BOX:
            return "Message Box";

        case SCREEN_COPY:
            return "Copy";

        case SCREEN_INITIATOR_MAIN:
            return "Initiator Main";
    }
    return "Unknown";
}

void changeScreen(SCREEN_TYPE type, int index)
{
    if (type == SCREEN_NONE)
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