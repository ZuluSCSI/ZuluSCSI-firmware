#if defined(CONTROL_BOARD) && !defined(ENABLE_AUDIO_OUTPUT_SPDIF)

/**
 * ZuluIDE™ - Copyright (c) 2024 Rabbit Hole Computing™
 *
 * ZuluIDE™ firmware is licensed under the GPL version 3 or any later version. 
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

// This has been modifed to:
// - remove std::string
// - allow for a blank on the left hand side of the scroller (used for labels)

#pragma once

#include <Adafruit_SSD1306.h>
#include "dimensions.h"
#include "control_global.h"


  class ScrollingText {
  public:
    ScrollingText();
    /**
       Sets the display text and prepares to display it during the next call to display.
     **/
    void SetToDisplay(const char* toDisp);
    /**
	Checks to see if the text should be updated and makes the updates to the text position variables.

	Returns: True if the text should be redrawn; false otherise.
    **/
    bool CheckAndUpdateScrolling(absolute_time_t now);

    void Display();

    void Reset();
    void ResetDelay();

    void SetCenterStationaryText(bool value);

    Rectangle bounds;
    Adafruit_SSD1306 *graph;
    int _font;

    
  private:
    char to_display[MAX_PATH_LEN];
    
    absolute_time_t startScrollingAfter;
    bool scrollText;
    bool reverseScroll;
    Size toDispSize;
    uint16_t imageNameOffsetPixels;
    bool isDirty;
    bool isStationaryText;
  };

#endif