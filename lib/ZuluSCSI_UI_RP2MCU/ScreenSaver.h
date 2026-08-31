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

#ifndef SCREENSAVER_H
#define SCREENSAVER_H

#define ENABLE_HIGHER_LOAD_SCREEN_SAVERS
#define ENABLE_LIGHTSPEED_SCREEN_SAVERS   

typedef enum
{
    SCREENSAVER_RANDOM,
    SCREENSAVER_BLANK,
	SCREENSAVER_RANDOM_DVDSTYLE_LOGO,
    SCREENSAVER_BOUNCING_DVDSTYLE_LOGO
#ifdef ENABLE_HIGHER_LOAD_SCREEN_SAVERS
    , SCREENSAVER_HORIZONTAL_SCROLLING_ICONS,
    SCREENSAVER_VERTICAL_RAINING_ICONS
#ifdef ENABLE_LIGHTSPEED_SCREEN_SAVERS
    ,SCREENSAVER_LIGHTSPEED
#endif
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