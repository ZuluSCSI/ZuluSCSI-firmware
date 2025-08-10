#if defined(CONTROL_BOARD) && !defined(ENABLE_AUDIO_OUTPUT_SPDIF)

#include "MainScreen.h"
#include "MessageBox.h"
#include "ZuluSCSI_log.h"

#include "cache.h"

void MainScreen::init(int index)
{
  Screen::init(index);

  // Find first active scsi ID
  if (index == -1 || _selectedDevice == -1) // TODO just a test: _selectedDevice == -1? does it do anything when StateChange() called?
  {
    _selectedDevice = -1;
    int i;
    for (i=0;i<8;i++)
    {
      if (g_devices[i].Active)
      {
        _selectedDevice = i;
        break;
      }
    }
  }
  else
  {
    _selectedDevice = index;
  }
  _deviceMap = &g_devices[_selectedDevice];
}

void MainScreen::draw()
{
  _display.setCursor(0,0);             
  _display.print(F("SCSI Map"));
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
}

void MainScreen::tick()
{
  Screen::tick();
}

void MainScreen::sdCardStateChange(bool cardIsPresent)
{
  _selectedDevice = -1; // reset it, so it's recalced
  _deviceMap = NULL;
}

void MainScreen::shortRotaryPress()
{
  if (_selectedDevice == -1)
  {
    return;
  }

  changeScreen(SCREEN_INFO, _selectedDevice);
}

void MainScreen::shortUserPress()
{
  
}

void MainScreen::shortEjectPress()
{
  if (_selectedDevice == -1)
  {
    return;
  }

  DeviceMap *map = &g_devices[_selectedDevice];

  if (!map->IsBrowsable)
  {
    _messageBox.setReturnScreen(SCREEN_MAIN);
    _messageBox.setText("-- Warning --", "Browsing not", "Supported...");
    changeScreen(MESSAGE_BOX, _selectedDevice);
  }
  else
  { 
    if (_deviceMap->BrowseScreenType == 0)
    {
        changeScreen(SCREEN_BROWSE, _selectedDevice);
    }
    else
    {
        changeScreen(SCREEN_BROWSE_FLAT, _selectedDevice);
    }
  }
}

void MainScreen::longUserPress()
{
  changeScreen(SCREEN_ABOUT, -1);
}

void MainScreen::longEjectPress()
{
  if (_selectedDevice == -1)
  {
    return;
  }

  DeviceMap *map = &g_devices[_selectedDevice];

  if (!map->IsBrowsable)
  {
    _messageBox.setReturnScreen(SCREEN_MAIN);
    _messageBox.setText("-- Warning --", "Browsing not", "Supported...");
    changeScreen(MESSAGE_BOX, _selectedDevice);
  }
  else
  { 
    if ((doesDeviceHaveAnyCategoryFiles(_selectedDevice) == 0 && !_deviceMap->HasDirs) && g_cacheActive)
    {
      _messageBox.setReturnScreen(SCREEN_MAIN);
      _messageBox.setText("-- Warning --", "No folders or", "categories...");
      changeScreen(MESSAGE_BOX, _selectedDevice);
    }
    else
    {
      changeScreen(SCREEN_BROWSE_TYPE, _selectedDevice); 
    }
  }
}

void MainScreen::rotaryChange(int direction)
{
  if (_selectedDevice == -1) // there aren't any, so just return (it would have been set to something other than -1 if there were)
  {
    return;
  }
  
  int i;
  bool found = false;

  if (direction == 1)
  {
    // Find next valid scsi id
    for (i=_selectedDevice+1; i<8;i++)    
    {
      if (g_devices[i].Active)
      {
        _selectedDevice = i;
        found = true;
        break;
      }
    }
    if (!found)
    {
      for (i=0; i<_selectedDevice;i++)    
      {
        if (g_devices[i].Active)
        {
          _selectedDevice = i;
          found = true;
          break;
        }
      }
    }
  }
  else // dir -1
  {
    // Find next valid scsi id
    for (i=_selectedDevice-1; i>=0;i--)    
    {
      if (g_devices[i].Active)
      {
        _selectedDevice = i;
        found = true;
        break;
      }
    }
    if (!found)
    {
      for (i=7; i>=_selectedDevice;i--)    
      {
        if (g_devices[i].Active)
        {
          _selectedDevice = i;
          found = true;
          break;
        }
      }
    }
  }

  if (found)
  {
      forceDraw();
  }
}

void MainScreen::drawSCSIItem(int x, int y, int index)
{
  DeviceMap *map = &g_devices[index];

  _display.setCursor(x+10, y+1);             
  _display.print((int)index); 


  if (_selectedDevice == index)
  {
    _display.drawBitmap(x, y, select, 8,8, WHITE);
  }

  if (map->Active)
  {
    const uint8_t *deviceIcon = getIconForType((S2S_CFG_TYPE)map->DeviceType, true);
    _display.drawBitmap(x+43, y, deviceIcon, 12,12, WHITE);

    _display.drawBitmap(x+25, y, icon_ledon, 12,12, WHITE);
    
  }
  else
  {
    _display.drawBitmap(x+25, y, icon_ledoff, 12,12, WHITE);
  }
}

#endif