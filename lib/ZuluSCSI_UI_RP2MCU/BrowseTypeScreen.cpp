#if defined(CONTROL_BOARD)

#include "BrowseTypeScreen.h"
#include "ZuluSCSI_log.h"
#include "ZuluSCSI_disk.h"

#include "cache.h"
#include "control_global.h"

#define MAX_LINES 5

void BrowseTypeScreen::init(int index)
{
  Screen::init(index);

  _cursorPos = 0;
  _selectedDevice = 0;
  _screenOffset = 0;

  _scsiId = index;
  _deviceMap = &g_devices[_scsiId];
  
  _totCats = g_totalCategories[_scsiId];

  _showFolder = _deviceMap->HasDirs;
}

void BrowseTypeScreen::draw()
{     
  _display->setCursor(0,0);             
  _display->print(F("Browse Type"));
  _display->drawLine(0,10,127,10, 1);

  int yOffset = 14;
  int xOffset = 0;

  int i;
  for (i=0;i<MAX_LINES;i++)
  {
    int ind = i + _screenOffset;
    if (ind < (_totCats+2))
    {
      drawCategory(xOffset, yOffset, ind);
      yOffset+=10;
    }
  }
}

void BrowseTypeScreen::shortUserPress()
{
  changeScreen(SCREEN_MAIN, -1);
}

void BrowseTypeScreen::shortEjectPress()
{
  _deviceMap->BrowseScreenType = _selectedDevice;

  if (_deviceMap->BrowseScreenType == 0)
  {
      changeScreen(SCREEN_BROWSE, _scsiId); 
  }
  else
  {
      changeScreen(SCREEN_BROWSE_FLAT, _scsiId); 
  }
}

void BrowseTypeScreen::shortRotaryPress()
{
  _deviceMap->BrowseScreenType = _selectedDevice;

  if (_deviceMap->BrowseScreenType == 0)
  {
      changeScreen(SCREEN_BROWSE, _scsiId); 
  }
  else
  {
      changeScreen(SCREEN_BROWSE_FLAT, _scsiId); 
  }
}

void BrowseTypeScreen::rotaryChange(int direction)
{
  if (_selectedDevice == -1) // there aren't any, so just return (it would have been set to something other than -1 if there were)
  {
    return;
  }

  int totalLines = _totCats + 2;

  if (direction == 1)
  {
    _selectedDevice++;
    if (_selectedDevice == totalLines)
    {
      _selectedDevice--;
    }
    else
    {
      _cursorPos++;
      if (_cursorPos == MAX_LINES)
      {
        _cursorPos--;
        _screenOffset++;
      }
    }
  }
  else // dir -1
  {
    _selectedDevice--;
    if (_selectedDevice == -1)
    {
      _selectedDevice++;
    }
    else
    {
      _cursorPos--;
      if (_cursorPos < 0)
      {
        _cursorPos++;
        _screenOffset--;
      }
    }
  }

  forceDraw();
}

void BrowseTypeScreen::drawCategory(int x, int y, int index)
{
  if (index == 0)
  {
    _display->setCursor(x+10, y+1);       
    _display->print("All (Folders)"); 
  }
  if (index == 1)
  {
    _display->setCursor(x+10, y+1);             
    _display->print("All (Flat)"); 
    _display->setCursor(x+110, y+1);      
    if (g_cacheActive)
    {       
      _display->print(_deviceMap->TotalFlatFiles); 
    }
    else
    {
      int tot;
      SDNavTotalFilesRecursive.TotalItemsRecursive(_deviceMap->RootFolder, tot);
      _display->print(tot); 
    }
  }
  if (index > 1)
  {
    char * catInfo = g_categoryCodeAndNames[_scsiId][index-2];
    _display->setCursor(x+10, y+1);             
    _display->print("Cat: ");
    _display->print(&catInfo[1]); 
    _display->setCursor(x+110, y+1);          
    _display->print(_deviceMap->TotalFilesInCategory[index-2]); 
  }

  if (_selectedDevice == index)
  {
    _display->drawBitmap(x, y, icon_select, 8,8, WHITE);
  }
}

#endif