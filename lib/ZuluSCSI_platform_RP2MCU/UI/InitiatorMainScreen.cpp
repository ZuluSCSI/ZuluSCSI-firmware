#if defined(CONTROL_BOARD) && !defined(ENABLE_AUDIO_OUTPUT_SPDIF)

#include "InitiatorMainScreen.h"
#include "ZuluSCSI_log.h"

void InitiatorMainScreen::init(int index)
{
  Screen::init(index);

  _scsiId = index;

  // Init the scroller

  // Set the scroller text
}

void InitiatorMainScreen::draw()
{ 
  _display.setCursor(0,0);             
  _display.print(F("Initiator Scanning..."));
  _display.drawLine(0,10,127,10, 1);

  int yOffset = 13;
  int xOffset = 0;

  int i;
  for (i=0;i<8;i++)
  {
    if (i == 4)
    {
      xOffset = 64;
      yOffset = 12;
    }

    drawSCSIItem(xOffset, yOffset, i);

    yOffset+=13;
  }

  return;

  _display.setCursor(0,0);             
  _display.print(F("Scanning..."));
  _display.drawLine(0,10,127,10, 1);
}


void InitiatorMainScreen::tick()
{
  /*
  switch(g_initiatorTransientData.Type)    
  {
      case INITIATOR_MSG_PROGRESS:
      {
          int speed_kbps = g_initiatorTransientData.SectorsInBatch * g_initiatorTransientData.SectorSize / g_initiatorTransientData.Elapsed;
          
          logmsg("*** Main Scren INITIATOR_MSG_PROGRESS SCSI read succeeded, sectors done: ",
              (int)g_initiatorTransientData.SectorsCopied, " / ", (int)g_initiatorTransientData.SectorCount,
              " speed ", speed_kbps, " kB/s - ", 
              (int)(100 * (int64_t)g_initiatorTransientData.SectorsCopied / g_initiatorTransientData.SectorCount), "%");

          int per = (int)(100 * (int64_t)g_initiatorTransientData.SectorsCopied / g_initiatorTransientData.SectorCount);
          if (per != _prevPer)
          {
            _prevPer = per;
            g_initiatorTransientData.Type = INITIATOR_MSG_NONE;
            forceDraw();
          }
          break;
      }
  }
  */

  if (g_initiatorTransientData.Type != INITIATOR_MSG_NONE)
  {
    g_initiatorTransientData.Type = INITIATOR_MSG_NONE;
    forceDraw();
  }
  
  Screen::tick();
}

void InitiatorMainScreen::drawSCSIItem(int x, int y, int index)
{
  DeviceMap *map = &g_devices[index];

  _display.setCursor(x+10, y+1);             
  _display.print((int)index); 

  bool showType = false;

  switch(map->InitiatorDriveStatus)
  {
    case INITIATOR_DRIVE_UNKNOWN:
      _display.drawBitmap(x+25, y, icon_question, 12,12, WHITE);
      break;

    case INITIATOR_DRIVE_PROBING:
      _display.drawBitmap(x+25, y, icon_ledoff, 12,12, WHITE);
      break;

    case INITIATOR_DRIVE_CLONABLE:
      _display.drawBitmap(x+25, y, icon_ledsemi, 12,12, WHITE);
      showType = true;
      break;
    
    case INITIATOR_DRIVE_CLONED:
      _display.drawBitmap(x+25, y, icon_ledon, 12,12, WHITE);
      showType = true;
      break;
  }

  if (showType)
  {
    const uint8_t *deviceIcon = getIconForType((S2S_CFG_TYPE)map->DeviceType, true);
    _display.drawBitmap(x+43, y, deviceIcon, 12,12, WHITE);
  }
}

#endif