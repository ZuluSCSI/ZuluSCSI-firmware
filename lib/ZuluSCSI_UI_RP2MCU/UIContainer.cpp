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

#include "ZuluSCSI_log.h"

#include "SystemMode.h"

#include "MainScreen.h"
#include "SplashScreen.h"
#include "BrowseTypeScreen.h"
#include "BrowseScreen.h"
#include "BrowseScreenFlat.h"
#include "InfoScreen.h"
#include "MessageBox.h"
#include "CopyScreen.h"
#include "InitiatorMainScreen.h"
#include "control.h"
#include <ZuluSCSI_buffer_control.h>


extern Screen *g_activeScreen;
extern int g_previousIndex;

SplashScreen *_splashScreen;
MainScreen *_mainScreen;
InfoScreen *_infoScreen;
BrowseScreen *_browseScreen;
BrowseTypeScreen *_browseTypeScreen;
BrowseScreenFlat *_browseScreenFlat;
MessageBox *_messageBox;
CopyScreen *_copyScreen;
InitiatorMainScreen *_initiatorMainScreen;

#if ZULUSCSI_RESERVED_SRAM_LEN == 0
extern Adafruit_SSD1306 allocated_g_display;

static SplashScreen static_splashScreen(&allocated_g_display);
static MainScreen static_mainScreen(&allocated_g_display);
static InfoScreen static_infoScreen(&allocated_g_display);
static BrowseScreen static_browseScreen(&allocated_g_display);
static BrowseTypeScreen static_browseTypeScreen(&allocated_g_display);
static BrowseScreenFlat static_browseScreenFlat(&allocated_g_display);
static MessageBox static_messageBox(&allocated_g_display);
static CopyScreen static_copyScreen(&allocated_g_display);
static InitiatorMainScreen static_initiatorMainScreen(&allocated_g_display);

void initScreens()
{
    _splashScreen = &static_splashScreen;
    _mainScreen = &static_mainScreen;
    _infoScreen = &static_infoScreen;
    _browseScreen = &static_browseScreen;
    _browseTypeScreen = &static_browseTypeScreen;
    _browseScreenFlat = &static_browseScreenFlat;
    _messageBox = &static_messageBox;
    _copyScreen = &static_copyScreen;
    _initiatorMainScreen = &static_initiatorMainScreen;
}

#else
extern Adafruit_SSD1306 *g_display;

void initScreens()
{
    _splashScreen = new (reserve_buffer_align(sizeof(SplashScreen), 4)) SplashScreen(g_display);
    _mainScreen = new (reserve_buffer_align(sizeof(MainScreen), 4)) MainScreen(g_display);
    _infoScreen = new (reserve_buffer_align(sizeof(InfoScreen), 4)) InfoScreen(g_display);
    _browseScreen = new (reserve_buffer_align(sizeof(BrowseScreen), 4)) BrowseScreen(g_display);
    _browseTypeScreen = new (reserve_buffer_align(sizeof(BrowseTypeScreen), 4)) BrowseTypeScreen(g_display);
    _browseScreenFlat = new (reserve_buffer_align(sizeof(BrowseScreenFlat), 4)) BrowseScreenFlat(g_display);
    _messageBox = new (reserve_buffer_align(sizeof(MessageBox), 4)) MessageBox(g_display);
    _copyScreen = new (reserve_buffer_align(sizeof(CopyScreen), 4)) CopyScreen(g_display);
    _initiatorMainScreen = new (reserve_buffer_align(sizeof(InitiatorMainScreen), 4)) InitiatorMainScreen(g_display);
}
#endif
// Call new card method on all screens
void sendSDCardStateChangedToScreens(bool cardIsPresent)
{
    _splashScreen->sdCardStateChange(cardIsPresent);
    _mainScreen->sdCardStateChange(cardIsPresent);
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

    // If no index, set it to the previous one. This is used to restore state to the previous screen.
    // e.g. going back to main with restore the selected SCSI id item in the main view
    if (index == -1)
    {
        index = g_previousIndex;
    }

    // Init the Screen
    g_activeScreen->init(index);

    // Store the index, incase the next screen change index is a -1
    g_previousIndex = index;
}

#endif