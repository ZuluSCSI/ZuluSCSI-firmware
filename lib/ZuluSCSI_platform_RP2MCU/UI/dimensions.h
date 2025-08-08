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

#pragma once
#include <cstdint>


  class Point {
  public:
    uint16_t x;
    uint16_t y;
  };

  class Size {
  public:
    uint16_t width;
    uint16_t height;
  };
  
  class Rectangle {
  public:
    Point topLeft;
    Size size;
    uint16_t Right() {
      return topLeft.x + size.width;
    }

    uint16_t Bottom() {
      return topLeft.y + size.height;
    }
    
    Rectangle MakeCentered(Size newSize) {
      auto newWidth = newSize.width > size.width ? size.width : newSize.width;
      uint16_t center = topLeft.x + (size.width - newWidth) / 2;
      auto newHeight = newSize.height > size.height ? size.height : newSize.height;
      uint16_t middle = topLeft.y + (size.height - newHeight) / 2;
      return { {center, middle}, {newWidth, newHeight}};
    }

    Rectangle MakeCenteredAt(uint16_t y, Size newSize) {
      auto newWidth = newSize.width > size.width ? size.width : newSize.width;
      uint16_t center = topLeft.x + (size.width - newWidth) / 2;
      auto newHeight = newSize.height > size.height ? size.height : newSize.height;
      return { {center, y}, {newWidth, newHeight}};
    }
  };

#endif