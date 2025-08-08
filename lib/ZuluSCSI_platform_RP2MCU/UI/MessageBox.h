#if defined(CONTROL_BOARD) && !defined(ENABLE_AUDIO_OUTPUT_SPDIF)

#ifndef MESSAGEBOX_H
#define MESSAGEBOX_H

#include "Screen.h"

class MessageBox : public Screen
{
public:
    MessageBox(Adafruit_SSD1306 &display) : Screen(display) {}

    void init(int index);
    void draw();
    void tick();
    
    void shortRotaryPress();
    void shortUserPress();
    void shortEjectPress();
    void rotaryChange(int direction);
    
    void setReturnScreen(SCREEN_TYPE ret);
    void setReturnConditionPendingLoadComplete();

    void setText(const char *title, const char *line1, const char *line2);

    bool clearScreenOnDraw();
private:
    int _index;
    bool _conditionPendingLoadComplete;
    SCREEN_TYPE _return;
    const char *_title;
    const char *_line1;
    const char *_line2;

    void drawText(Rectangle bound, int y, const char *msg);
};

extern MessageBox _messageBox;

#endif

#endif