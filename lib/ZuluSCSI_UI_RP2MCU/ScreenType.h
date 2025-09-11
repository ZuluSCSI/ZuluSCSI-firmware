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

#ifndef SCREENTYPE_H
#define SCREENTYPE_H

typedef enum
{
    SCREEN_NONE,
	SCREEN_SPLASH,
    SCREEN_MAIN,
    SCREEN_INFO,
    SCREEN_INFO_PAGE2,
    SCREEN_INFO_PAGE3,
    SCREEN_BROWSE_TYPE,
    SCREEN_BROWSE,
    SCREEN_BROWSE_FLAT,
    MESSAGE_BOX,
    SCREEN_COPY,
    SCREEN_INITIATOR_MAIN
} SCREEN_TYPE;

#endif

#endif