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