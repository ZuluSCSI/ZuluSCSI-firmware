#if defined(CONTROL_BOARD) && !defined(ENABLE_AUDIO_OUTPUT_SPDIF)

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