#if defined(CONTROL_BOARD) && !defined(ENABLE_AUDIO_OUTPUT_SPDIF)

#include "CopyScreen.h"
#include "ZuluSCSI_log.h"

#define MIN_UPDATE_TIME 500

void CopyScreen::setShowRetriesAndErrors(bool showRetriesAndErrors)
{
  _showRetriesAndErrors = showRetriesAndErrors;
}
void CopyScreen::init(int index)
{
  Screen::init(index);

  _scsiId = index;

  _progressBar.bounds = Rectangle{{0,18}, {90, 11}};
  // Init the scroller
  _firstBlock = true;
  
  _nextScreenUpdate = make_timeout_time_ms(MIN_UPDATE_TIME);

#ifdef SCREEN_SHOTS
    _screenShotTaken = false;
#endif
}

void CopyScreen::draw()
{ 
  _display.setCursor(0,0);             
  _display.print(_bannerText);
  
  _iconX = 92;
  if (DeviceType != 255)
  {
    const uint8_t *deviceIcon = getIconForType((S2S_CFG_TYPE)DeviceType, true);
    
    drawIconFromRight(deviceIcon, 6, 0);
  }

  if (_scsiId != 255)
  {
    _display.setTextSize(2);             
    _display.setCursor(112,0);       
    _display.print(_scsiId);           
    _display.setTextSize(1);      
  }
  else
  {
    _iconX += 16;
  }

  _display.drawLine(0,10,_iconX+11,10, 1);

  // Progress and Percentage
  _progressBar.Display();

  _display.setCursor(91,20);       
  dtostrf(_per, 5, 1, _buff);
  _display.print(_buff);           
  _display.print("%");           

  // Retries and Error
  if (_showRetriesAndErrors)
  {
    _display.setCursor(0,32);       
    _display.print("Ret: ");           
    _display.print(TotalRetries);           
    _display.setCursor(68,32);       
    _display.print("Err: ");           
    _display.print(TotalErrors);      
  }
  if (_showInfoText)
  {
    _display.setCursor(0,32);       
    _display.print(_infoText);           
  }

  // TIME

  float ela = ((float)millis() - (float)_startTime ) / 1000.0;
  makeTimeStr((int)ela, _buff);

  _display.setCursor(68,45);       
  _display.print("E:");           
  _display.print(_buff);           

  float tot = ((float)ela / (float)BlocksCopied) * (float)BlockCount;
  float rem = tot - ela;
  makeTimeStr((int)rem, _buff);

  _display.setCursor(68,56);      
  _display.print("R:");            
  _display.print(_buff);  

    // Speed
  _display.setCursor(0,45);
  if (BlockTime > 0)
  {
    int speed_kbps = BlocksInBatch * BlockSize / BlockTime;
    speed_kbps *= 1000; // BlockTime is in ms

    makeImageSizeStr(speed_kbps , _buff);
    _display.print(_buff);           
  }
  else
  {
    _display.print("  0");
  }
  _display.print("Bs");
  
 
  // Remaiing
  //u_int64_t copied = (SectorsCopied * BlockSize);
  //u_int64_t total = (SectorCount * BlockSize);
  //u_int64_t remaining = total - copied;

  u_int64_t remaining = (BlockCount - BlocksCopied) * BlockSize;
 
  _display.setCursor(0,56);       
  makeImageSizeStr(remaining , _buff);
  _display.print("R: ");           
  _display.print(_buff);       
  
#ifdef SCREEN_SHOTS
    if (!_screenShotTaken)
    {
      if (_per > 21)
      {
        saveScreenShot();
        _screenShotTaken = true;
      }

    }
#endif
}

void CopyScreen::tick()
{
  if (NeedsProcessing)
  {
    NeedsProcessing = false;
  
    if (_firstBlock)
    {
      _firstBlock = false;
      _startTime = millis();
    }
    _per = (float)(100.0 * ((float)BlocksCopied / (float)BlockCount));
    if (_progressBar.SetPercent(_per))
    {
      forceDraw();
    }
  }

  int64_t togo = absolute_time_diff_us (get_absolute_time(), _nextScreenUpdate);
  if (togo < 0)
  {
    _nextScreenUpdate = make_timeout_time_ms(MIN_UPDATE_TIME);
    forceDraw();
  }

  Screen::tick();
}

void CopyScreen::setBannerText(const char *text)
{
  strcpy(_bannerText, text);
}

void CopyScreen::setInfoText(const char *text)
{
  strcpy(_infoText, text);
}

void CopyScreen::setShowInfoText(bool showInfoText)
{
  _showInfoText = showInfoText;
}

void CopyScreen::shortUserPress()
{
  // changeScreen(SCREEN_MAIN, -1);
}

#endif