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

#include "ProgressBar.h"
#include "ZuluSCSI_log.h"

ProgressBar::ProgressBar() : graph(NULL)
{
     _minUpdate = 0.1;
     _displayDP = 1;
     _percent = -1;
}

bool ProgressBar::SetPercent(float percentage)
{
    float dif = _percent - percentage;
    if (dif < 0)
    {
        dif = -dif;
    }

    if (dif < _minUpdate)
    {
        return false;
    }

    _percent = percentage;

    dtostrf(_percent, 4 +_displayDP, _displayDP, to_display);
        
    int16_t x = 0, y = 0;
    graph->getTextBounds(to_display, 0 ,0, &x, &y, &toDispSize.width, &toDispSize.height);
    return true;
}
    
void ProgressBar::Display()
{
    graph->setTextColor(1);
    graph->drawRect(bounds.topLeft.x, bounds.topLeft.y, bounds.size.width, bounds.size.height, 1);

    float scale = _percent / 100.0;

    if (scale > 0)
    {
        int width = (bounds.size.width - bounds.topLeft.x)-2;
        int line = (int)((float)width * scale) + 1;

        graph->fillRect(bounds.topLeft.x+1, bounds.topLeft.y+1, line, bounds.size.height-2, 1);
    }
}


#endif