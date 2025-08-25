#if defined(CONTROL_BOARD) && !defined(ENABLE_AUDIO_OUTPUT_SPDIF)

#include "InitiatorDriveScreen.h"
#include "ZuluSCSI_log.h"

#define MIN_UPDATE_TIME 500

void InitiatorDriveScreen::init(int index)
{
  Screen::init(index);

  _scsiId = index;

  _progressBar.bounds = Rectangle{{0,18}, {90, 11}};
  // Init the scroller
  _firstBlock = true;
  
  _nextScreenUpdate = make_timeout_time_ms(MIN_UPDATE_TIME);
}

void InitiatorDriveScreen::draw()
{ 
  _display.setCursor(0,0);             
  _display.print(F("Cloning"));
  
  _iconX = 92;

  DeviceMap &map = g_devices[_scsiId];

  const uint8_t *deviceIcon = getIconForType((S2S_CFG_TYPE)map.DeviceType, true);
  drawIconFromRight(deviceIcon, 6, 0);

  if (map.IsRaw)
  {
    drawIconFromRight(icon_raw, 0, 0);
  }
  if (map.IsRom)
  {
    drawIconFromRight(icon_rom, 0, 0);
  }

  _display.drawLine(0,10,_iconX+11,10, 1);

  _display.setTextSize(2);             
  _display.setCursor(112,0);       
  _display.print(_scsiId);           
  _display.setTextSize(1);      


  // Progress and Percentage
  _progressBar.Display();

  _display.setCursor(91,20);       
  dtostrf(_per, 5, 1, _buff);
  _display.print(_buff);           
  _display.print("%");           

  // Retries and Error
  _display.setCursor(0,32);       
  _display.print("Ret: ");           
  _display.print(map.TotalRetries);           
  _display.setCursor(68,32);       
  _display.print("Err: ");           
  _display.print(map.TotalErrors);      


  // TIME

  float ela = ((float)millis() - (float)_startTime ) / 1000.0;
  makeTimeStr((int)ela, _buff);

  _display.setCursor(68,45);       
  _display.print("E:");           
  _display.print(_buff);           

  float tot = ((float)ela / (float)g_initiatorTransientData.SectorsCopied) * (float)map.SectorCount;
  float rem = tot - ela;
  makeTimeStr((int)rem, _buff);

  _display.setCursor(68,56);      
  _display.print("R:");            
  _display.print(_buff);  

    // Speed
  int speed_kbps = g_initiatorTransientData.SectorsInBatch * map.SectorSize / g_initiatorTransientData.BlockTime;
  speed_kbps *= 1000; // BlockTime is in ms

  makeImageSizeStr(speed_kbps , _buff);
  
  _display.setCursor(0,45);       
  _display.print(_buff);           
  _display.print("Bs");
 
  // Remaiing
  //u_int64_t copied = (g_initiatorTransientData.SectorsCopied * map.SectorSize);
  //u_int64_t total = (map.SectorCount * map.SectorSize);
  //u_int64_t remaining = total - copied;

  u_int64_t remaining = (map.SectorCount - g_initiatorTransientData.SectorsCopied) * map.SectorSize;
 
  _display.setCursor(0,56);       
  makeImageSizeStr(remaining , _buff);
  _display.print("R: ");           
  _display.print(_buff);           
}

void InitiatorDriveScreen::tick()
{
  DeviceMap &map = g_devices[_scsiId];

  switch(g_initiatorTransientData.Type)    
  {
      case INITIATOR_MSG_PROGRESS:
      {
        if (_firstBlock)
        {
          _firstBlock = false;
          _startTime = millis();
        }
        _per = (float)(100.0 * ((float)g_initiatorTransientData.SectorsCopied / (float)map.SectorCount));
        if (_progressBar.SetPercent(_per))
        {
          forceDraw();
        }
        break;
      }
  }

  int64_t togo = absolute_time_diff_us (get_absolute_time(), _nextScreenUpdate);
  if (togo < 0)
  {
    _nextScreenUpdate = make_timeout_time_ms(MIN_UPDATE_TIME);
    forceDraw();
  }

  // At the end of a tick, it's assumed that message is processed, so set to none
  if (g_initiatorTransientData.Type != INITIATOR_MSG_NONE)
  {
    g_initiatorTransientData.Type = INITIATOR_MSG_NONE;
  }

  Screen::tick();
}


void InitiatorDriveScreen::shortUserPress()
{
  changeScreen(SCREEN_MAIN, -1);
}

#endif