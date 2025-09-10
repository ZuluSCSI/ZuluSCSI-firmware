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

#include "scrolling_text.h"
#include "ZuluSCSI_log.h"

#ifndef SCROLL_INTERVAL_MS
#define SCROLL_INTERVAL_MS 60
#endif

#ifndef SCROLL_START_DELAY_MS
#define SCROLL_START_DELAY_MS 1000
#endif

ScrollingText::ScrollingText() : // toDisplay(""),
								    graph(NULL),
								    startScrollingAfter(at_the_end_of_time),
								    scrollText(false),
								    reverseScroll(false),
								    isDirty(false),
								    isStationaryText(false)
{
}

void ScrollingText::SetToDisplay(const char* toDisp) 
{
  if (strcmp(to_display, toDisp) == 0)
  {
    return;
  }

 // toDisplay = std::string(toDisp);
  strcpy(to_display, toDisp);

  int16_t x = 0, y = 0;
  graph->getTextBounds(to_display, 0 ,0, &x, &y, &toDispSize.width, &toDispSize.height);

  isStationaryText = toDispSize.width <= bounds.size.width;

  // When changing the text, don't start scrolling immediately.
  Reset();
}

void ScrollingText::ResetDelay() 
{
  startScrollingAfter = make_timeout_time_ms(SCROLL_START_DELAY_MS);
}
void ScrollingText::Reset() 
{
  startScrollingAfter = make_timeout_time_ms(SCROLL_START_DELAY_MS);
  scrollText = false;
  reverseScroll = false;
  imageNameOffsetPixels = 0;
  isDirty = true;
}

bool ScrollingText::CheckAndUpdateScrolling(absolute_time_t now) 
{
  // Return false
  if (isStationaryText || (!scrollText && absolute_time_diff_us (now, startScrollingAfter) > 0)) 
  {
    auto returnValue = isDirty;
    isDirty = false;
    return returnValue;
  }

  // Update the offset prior to calling display.
  scrollText = true;
  if (reverseScroll) 
  {
    imageNameOffsetPixels-=3;
    if (imageNameOffsetPixels <= 0) 
    {
      scrollText = false;
      reverseScroll = false;
      startScrollingAfter = make_timeout_time_ms(SCROLL_START_DELAY_MS);
    }
  } 
  else
  {
    imageNameOffsetPixels+=3;

    if (imageNameOffsetPixels >= toDispSize.width - bounds.size.width) 
    {
      // The text scrolled too far, reset.
      reverseScroll = true;
      scrollText = false;
      startScrollingAfter = make_timeout_time_ms(SCROLL_START_DELAY_MS);
      return false;
    }
  }

  return true;
}

void ScrollingText::Display() 
{
  graph->setTextSize(_font);  
  if (isStationaryText) 
  {
    //auto dispBox = bounds.MakeCentered(toDispSize);
    graph->setCursor(bounds.topLeft.x, bounds.topLeft.y);
    graph->print(to_display);
  } 
  else
  {
    auto left = bounds.topLeft.x - imageNameOffsetPixels;
    // Move the cursor.
    graph->setCursor(left, bounds.topLeft.y);

    // Print the text
    graph->print(to_display);

    // As the display scrolls across the whole line, if you want a label on the left and the x bound > 0
    // then draw a fill blanked rectangle to blank out the text on the left to leave space
    if (bounds.topLeft.x > 0)
    {
      // Now blank the left
      graph->fillRect(0, bounds.topLeft.y, bounds.topLeft.x-1, bounds.size.height, 0);
    }
  }
}

#endif