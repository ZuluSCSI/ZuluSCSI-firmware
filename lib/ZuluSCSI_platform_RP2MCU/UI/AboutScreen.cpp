#if defined(CONTROL_BOARD) && !defined(ENABLE_AUDIO_OUTPUT_SPDIF)

#include "AboutScreen.h"
#include "ZuluSCSI_log.h"

#include "control_global.h"

void AboutScreen::draw()
{
  _display.drawBitmap(0,0, icon_zuluscsi, 128,64, WHITE);
  _display.setCursor(0,56);             
  _display.print(g_log_short_firmwareversion);
}

void AboutScreen::shortUserPress()
{
    changeScreen(SCREEN_MAIN, -1);
}

#endif