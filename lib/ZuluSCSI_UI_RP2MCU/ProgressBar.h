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

#ifndef PROGRESSBAR_H
#define PROGRESSBAR_H

#include <Adafruit_SSD1306.h>
#include "dimensions.h"
#include "control_global.h"

class ProgressBar {
  public:
    ProgressBar();

    bool SetPercent(float percentage);
    
    void Display();

    Rectangle bounds;
    Adafruit_SSD1306 *graph;
    int _font;

    int _displayDP;
    float _minUpdate;
    
  private:
    char to_display[10];
    float _percent;
    Size toDispSize;

  };


#endif

#endif