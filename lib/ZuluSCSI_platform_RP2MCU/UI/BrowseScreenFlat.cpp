#if defined(CONTROL_BOARD) && !defined(ENABLE_AUDIO_OUTPUT_SPDIF)

#include "BrowseScreenFlat.h"
#include "ZuluSCSI_log.h"
#include "ZuluSCSI_disk.h"
#include "MessageBox.h"

#include "cache.h"
#include "control_global.h"

void BrowseScreenFlat::init(int index)
{
  Screen::init(index);

  initScrollers(2);

  setupScroller(0, 42, 22, 88, 8, 1);
  setupScroller(1, 42, 36, 88, 8, 1);

  _scsiId = index;
  _deviceMap = &g_devices[_scsiId];

  _currentObjectIndex = 0;

  if (g_pendingLoadComplete > -1)
  {
    // We have return to the Browser from the MessageBox during an image load
    // so Patch the device now
    patchDevice(g_pendingLoadComplete);
    g_pendingLoadComplete = -1;
  }

  if (_deviceMap->BrowseScreenType == 1)
  {
    if (g_cacheActive)
    {
      _totalObjects = _deviceMap->TotalFlatFiles;
      _catChar = '_';
    }
    else
    {
      _totalObjects = totalFilesRecursiveInDir(_scsiId, _deviceMap->RootFolder);
    }
  }
  else
  {
    // Note, if cache is not active, we can't get here as categoies will be empty
    int catCategory = _deviceMap->BrowseScreenType-2;
    _totalObjects = _deviceMap->TotalFilesInCategory[catCategory];
     
    _catChar = g_categoryCodeAndNames[_scsiId][catCategory][0];
  }

  // Find the index of the file 
  int i;
  u_int64_t size;

  for (i=0;i<_totalObjects;i++)
  {
    if (g_cacheActive)
    {
      getCacheFile(_scsiId, _catChar, i, g_tmpFilename, g_tmpFilepath, size);
    }
    else
    {
      findFilesecursiveByIndex(_scsiId, _deviceMap->RootFolder, i, g_tmpFilename, g_tmpFilepath, MAX_PATH_LEN, size);
    }
    
    if (strcmp(_deviceMap->Filename, g_tmpFilename) == 0 && strcmp(_deviceMap->Path, g_tmpFilepath) == 0)
    {
      _currentObjectIndex = i;
      break;
    }
  }
  
  getCurrentFilenameAndUpdateScrollers();
}

void BrowseScreenFlat::draw()
{   
  _display.setCursor(0,0);             
  _display.print(F("Flat Browser"));

  _iconX = 92;
  
  DeviceMap &map = g_devices[_scsiId];

  const uint8_t *deviceIcon = getIconForType((S2S_CFG_TYPE)map.DeviceType, true);
  drawIconFromRight(deviceIcon, 6, 0);

 _display.drawLine(0,10,_iconX+11,10, 1);       

   _display.setTextSize(2);             
  _display.setCursor(112,0);       
  _display.print(_scsiId);           
  _display.setTextSize(1);            

  _display.setCursor(0,36);             
  _display.print(F("CWD: "));     

  _display.setCursor(0,22);             
  _display.print(F("Item: "));

  _display.setCursor(0,46);             
  _display.print(F("Size: "));
  _display.setCursor(42,46);    
    
  makeImageSizeStr(_currentObjectSize, _sizeBuffer);
  _display.print(_sizeBuffer);

  _display.setCursor(0,56);    
  _display.print(F("File"));
  
  _display.setCursor(42,56);    
  _display.print((_currentObjectIndex+1));
  _display.print(" of ");
  _display.print(_totalObjects);
}

void BrowseScreenFlat::shortRotaryPress()
{
  resetScrollerDelay();
}

void BrowseScreenFlat::shortUserPress()
{
  resetScrollerDelay();
  changeScreen(SCREEN_MAIN, -1);
}

void BrowseScreenFlat::shortEjectPress()
{
  resetScrollerDelay();
  loadSelectedImage();
}

void BrowseScreenFlat::rotaryChange(int direction)
{
  resetScrollerDelay();
  if (direction == 1)
  {
    navigateToNextObject();
  }
  else if (direction == -1)
  {
    navigateToPreviousObject();
  }
}

// UI

int BrowseScreenFlat::navigateToNextObject()
{
    _currentObjectIndex++;
    if (_currentObjectIndex == _totalObjects)
    {
        _currentObjectIndex = 0;
    }

    getCurrentFilenameAndUpdateScrollers();
    forceDraw();

    return 1;
}

int BrowseScreenFlat::navigateToPreviousObject()
{
    _currentObjectIndex--;
    if (_currentObjectIndex == -1)
    {
        _currentObjectIndex = _totalObjects-1;
    }

    getCurrentFilenameAndUpdateScrollers();
    forceDraw();

    return 1;
}

void BrowseScreenFlat::getCurrentFilenameAndUpdateScrollers()
{
  getCurrentFilename();
  setScrollerText(0, _currentObjectName);
  setScrollerText(1, _currentObjectPath);
}

// Backend

void BrowseScreenFlat::getCurrentFilename()
{
    if (_currentObjectIndex < _totalObjects) // Object from disc
    {
        if (g_cacheActive)
        {
          getCacheFile(_scsiId, _catChar, _currentObjectIndex, _currentObjectName, _currentObjectPath, _currentObjectSize);
        }
        else
        {
          findFilesecursiveByIndex(_scsiId, _deviceMap->RootFolder, _currentObjectIndex, _currentObjectName, _currentObjectPath, 64, _currentObjectSize);
        }
    }
}

void BrowseScreenFlat::loadSelectedImage()
{
    strcpy(g_tmpFilepath, _currentObjectPath);
    strcat(g_tmpFilepath, "/");
    strcat(g_tmpFilepath, _currentObjectName);
    
    haltUIUpdates();
    if (loadImageDeferred(_scsiId, g_tmpFilepath, SCREEN_BROWSE_FLAT, _scsiId))
    {
      strcpy(_deviceMap->Path, _currentObjectPath);
      strcpy(_deviceMap->Filename, _currentObjectName);
    }
}

#endif