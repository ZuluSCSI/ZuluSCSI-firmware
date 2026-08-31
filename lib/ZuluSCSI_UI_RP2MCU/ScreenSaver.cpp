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

#include "ScreenSaver.h"

#include <Adafruit_SSD1306.h>

#include "ZuluSCSI_settings.h"
#include "Screen.h"
#include "SystemMode.h"
#include "ZuluSCSI_log.h"

#include "Vec2D.h"

extern Adafruit_SSD1306 *g_display;
extern Screen *g_activeScreen;
extern void enableScreen(bool state);
extern SYSTEM_MODE g_systemMode;

// Screen saver
bool screenSaverEnabled = false;
long screenSaverStartTime = -1;
bool screenSaverTimerStarted = false;
bool g_screenSaverActive  = false;
bool g_userInputDetected = false;
int currentSelection;

long nextScreenSaverChange;
int scrX = 5;
int scrY = 5;
bool dirX = true;
bool dirY = true;

#ifdef ENABLE_HIGHER_LOAD_SCREEN_SAVERS

#ifdef ENABLE_LIGHTSPEED_SCREEN_SAVERS
#define MAX_OBJECT_MANY 32
#define TOTAL_SCREEN_SAVER 6
#else
#define MAX_OBJECT_MANY 8
#define TOTAL_SCREEN_SAVER 5
#endif

#define MAX_OBJECT_FEW 8

uint8_t sT[MAX_OBJECT_MANY];  // Icon Type
uint8_t size[MAX_OBJECT_MANY]; // start size
bool sA[MAX_OBJECT_MANY]; // Active?
Vec2D vectors[MAX_OBJECT_MANY]; // direction
Vec2D points[MAX_OBJECT_MANY]; // position

#else // !ENABLE_HIGHER_LOAD_SCREEN_SAVERS
#define TOTAL_SCREEN_SAVER 3
#endif // ENABLE_HIGHER_LOAD_SCREEN_SAVERS


const uint8_t *GetIconByIndex(int index)
{
    switch(index)
    {
        case 0:
            return icon_cd;

        case 1:
            return icon_floppy;

        case 2:
            return icon_removable;

        case 3:
            return icon_fixed;

        case 4:
            return icon_mo;

        case 5:
            return icon_network;

        case 6:
            return icon_tape;

        case 7:
            return icon_zip;
        
        case 8:
            return icon_rom;

        case 9:
            return icon_sd;

        case 10:
            return icon_nosd;

    }
    return icon_cd;
}

void drawScreenSaver()
{
    if (millis() > nextScreenSaverChange)
    {
        if (currentSelection != SCREENSAVER_BLANK) // i.e. not blank
        {
            g_display->clearDisplay();

            switch(currentSelection)
            {
                case SCREENSAVER_RANDOM_DVDSTYLE_LOGO:
                {
                    scrX = random(64);
                    scrY = random(33);
                    g_display->drawBitmap(scrX,scrY, icon_zuluscsi_mini,64,31 , WHITE);
                    break;
                }
                case SCREENSAVER_BOUNCING_DVDSTYLE_LOGO:
                {
                    scrX += dirX?1:-1;
                    scrY += dirY?1:-1;
                    if (scrX > 62  || scrX<1)
                    {
                        dirX = !dirX;
                    }
                    if (scrY > 30  || scrY<1)
                    {
                        dirY = !dirY;
                    }
                    g_display->drawBitmap(scrX,scrY, icon_zuluscsi_mini,64,31 , WHITE);
                    break;
                }
#ifdef ENABLE_HIGHER_LOAD_SCREEN_SAVERS
                case SCREENSAVER_HORIZONTAL_SCROLLING_ICONS:
                case SCREENSAVER_VERTICAL_RAINING_ICONS:
                {
                    int i;
                    int index = -1;
                    // look for inactive
                    for (i=0;i<MAX_OBJECT_FEW;i++)
                    {
                        if (!sA[i])
                        {
                            index = i;
                            break;
                        }
                    }

                    // Found an inactive
                    if (index > -1)
                    {
                        if (random(10) == 0)
                        {
                            sA[index] = true;

                            if (currentSelection == SCREENSAVER_HORIZONTAL_SCROLLING_ICONS)
                            {
                                points[index].set(-12, random(54));
                                vectors[index].set(random(3)+1, 0);
                            }
                            else
                            {
                                points[index].set(random(116), -12);
                                vectors[index].set(0, random(3)+1);
                            }
                            sT[index] = random(11);
                        }
                    }

                    for (i=0;i<MAX_OBJECT_FEW;i++)
                    {
                     if (sA[i])
                     {
                        points[i] += vectors[i];

                        if (currentSelection == SCREENSAVER_HORIZONTAL_SCROLLING_ICONS)
                        {
                            if (points[i].x > 127)
                            {
                                sA[i] = false;
                            }
                        }
                        else
                        {
                            if (points[i].y > 63)
                            {
                                sA[i] = false;
                            }
                        }
                     }
                    }

                    for (i=0;i<MAX_OBJECT_FEW;i++)
                    {
                     if (sA[i])
                     {
                        g_display->drawBitmap(points[i].x, points[i].y, GetIconByIndex(sT[i]),12,12 , WHITE);
                     }
                    }
                    break;
                }
#ifdef ENABLE_LIGHTSPEED_SCREEN_SAVERS
                case SCREENSAVER_LIGHTSPEED:
                {
                    int i;
                    int index = -1;
                    // look for inactive
                    for (i=0;i<MAX_OBJECT_MANY;i++)
                    {
                        if (!sA[i])
                        {
                            index = i;
                            break;
                        }
                    }

                    // Found an inactive
                    if (index > -1)
                    {
                        sA[index] = true;
                        size[index] = random(4);
                        points[index].set(64, 32);
                        
                        float tX, tY;
                        if (random(2) == 0)
                        {
                            tX = (random(2)==0) ? 127 : 0;
                            tY = random(76)-12;
                        }
                        else
                        {
                            tX = random(140)-12;
                            tY = (random(2)==0) ? 63 : 0;
                        }

                        Vec2D vec = Vec2D(points[index].x, points[index].y, tX, tY);
                        vectors[index].set(vec.normalized());
                        vectors[index] *= 0.5;
                        sT[index] = random(11);
                    }

                    for (i=0;i<MAX_OBJECT_MANY;i++)
                    {
                     if (sA[i])
                     {
                        vectors[i] *= 1.1; // speed it up a little
                        
                        points[i] += vectors[i];

                        if (points[i].x > 127 || points[i].x < -12)
                        {
                            sA[i] = false;
                        }
                        if (points[i].y > 63 || points[i].y < -12)
                        {
                            sA[i] = false;
                        }
                     }
                    }

                    for (i=0;i<MAX_OBJECT_MANY;i++)
                    {
                     if (sA[i])
                     {
                        if (size[i] == 0)
                        {
                            g_display->fillCircle((int)points[i].x, (int)points[i].y, 1, WHITE);
                            
                        }
                        else
                        {
                            g_display->drawPixel((int)points[i].x, (int)points[i].y, WHITE);
                        }
                     }
                    }
                    break;
                }
#endif
#endif
                default:
                    break;
            }
            
            g_display->display();
        }

        int delay = 0;
        switch(currentSelection)
        {
            case SCREENSAVER_RANDOM_DVDSTYLE_LOGO:
                delay = 5000;
                break;
            
            case SCREENSAVER_BOUNCING_DVDSTYLE_LOGO:
                delay = 200;
                break;

#ifdef ENABLE_HIGHER_LOAD_SCREEN_SAVERS
            case SCREENSAVER_HORIZONTAL_SCROLLING_ICONS:
            case SCREENSAVER_VERTICAL_RAINING_ICONS:
                delay = 40;
                break;

#ifdef ENABLE_LIGHTSPEED_SCREEN_SAVERS
            case SCREENSAVER_LIGHTSPEED:
                delay = 40;
                break;
#endif        
#endif
        }
        nextScreenSaverChange = millis() + delay;
    }
}

void enterScreenSaver()
{
#ifdef ENABLE_HIGHER_LOAD_SCREEN_SAVERS
    int i;
    for (i=0;i<MAX_OBJECT_MANY;i++)
    {
        sA[i] = false;
    }
#endif

    scsi_system_settings_t *cfg = g_scsi_settings.getSystem();
    currentSelection = cfg->controlBoardScreenSaverStyle;
    if (currentSelection == SCREENSAVER_RANDOM)
    {
        currentSelection = random(TOTAL_SCREEN_SAVER)+1;
    }

    enableScreen(currentSelection != SCREENSAVER_BLANK);
    
    nextScreenSaverChange = 0;
}

void exitScreenSaver()
{
    enableScreen(true);
    if (g_activeScreen != NULL)
    {
        g_activeScreen->forceDraw();
    }
}

void screenSaverLoop()
{
    scsi_system_settings_t *cfg = g_scsi_settings.getSystem();
    if (g_systemMode == SYSTEM_NORMAL && cfg->controlBoardScreenSaverTimeSec > 0)
    {
        if (!g_screenSaverActive)
        {
            // Screen saver timer hasn't been started
            if (!screenSaverTimerStarted)
            {
                if (!g_userInputDetected)
                {
                    screenSaverTimerStarted = true;
                    screenSaverStartTime = millis();
                }
            }
            else // Screen saver timer has started
            {
                if (g_userInputDetected) // but there was user input
                {
                    // So cancel the timer
                    screenSaverTimerStarted = false;
                }
                else
                {
                    // no user input, so check timers
                    long elaFromScreenSaverStartTime = millis() - screenSaverStartTime;
                    if (elaFromScreenSaverStartTime > (cfg->controlBoardScreenSaverTimeSec * 1000)) 
                    {
                        // Screen saver activated
                        g_screenSaverActive = true;
                        screenSaverTimerStarted = false;

                        enterScreenSaver();
                    }
                }
            }
        }
        else // screen saver is active
        {
            if (g_userInputDetected)
            {
                // Screen saver deactivated
                g_screenSaverActive = false;
                exitScreenSaver();
            }
        }
    }
}

#endif