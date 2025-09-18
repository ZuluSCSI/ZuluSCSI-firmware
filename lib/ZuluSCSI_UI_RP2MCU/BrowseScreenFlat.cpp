/**
 * Copyright (c) 2025 Guy Taylor
 *
 * ZuluSCSI™ firmware is licensed under the GPL version 3 or any later version.
 *
 * https://www.gnu.org/licenses/gpl-3.0.html
 * ----
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
**/

#if defined(CONTROL_BOARD)

#include "BrowseScreenFlat.h"
#include "ZuluSCSI_log.h"
#include "ZuluSCSI_disk.h"
#include "MessageBox.h"

#include "cache.h"
#include "control_global.h"
#include "UISDNavigator.h"

void BrowseScreenFlat::initImgDir(int index)
{
  if (_deviceMap->BrowseScreenType == SCREEN_BROWSETYPE_FLAT) // Not a category screen (they are > 1)
  {
    if (g_cacheActive)
    {
      _totalObjects = _deviceMap->TotalFlatFiles;
      _catChar = '_';
    }
    else
    {
      _totalObjects = _deviceMap->TotalFlatFiles;
    }
  }
  else
  {
    // Note, if cache is not active, we can't get here as categoies will be empty
    int catCategory = _deviceMap->BrowseScreenType-SCREEN_BROWSETYPE_CATEGORY;
    _totalObjects = _deviceMap->TotalFilesInCategory[catCategory];
     
    _catChar = g_categoryCodeAndNames[_scsiId][catCategory][0];
  }

  // Find the index of the file 
  if (g_cacheActive)
  {
    _currentObjectIndex = findCacheFile(_scsiId, _catChar, _currentObjectName, _currentObjectPath);
  }
  else
  {
    _currentObjectIndex = SDNavFindItemIndexByNameAndPathRecursive.FindItemIndexByNameAndPathRecursive(_deviceMap->RootFolder, _currentObjectName, _currentObjectPath);
    if (_currentObjectIndex == -1)
    {
      _currentObjectIndex = 0;
    }
  }
}

void BrowseScreenFlat::initImgX(int index)
{
  _totalObjects = _deviceMap->MaxImgX;

  // Find the index of the file 
  int i;
  u_int64_t size;

  strcpy(_currentObjectPath, "");
  
  for (i=0;i<_totalObjects;i++)
  {
    getImgXByIndex(_scsiId, i, g_tmpFilename, MAX_PATH_LEN, size);

    if (strcmp(_deviceMap->Filename, g_tmpFilename) == 0)
    {
      _currentObjectIndex = i;
      break;
    }
  }
}

void BrowseScreenFlat::initPrefix(int index)
{
  char pre[4];
  strcpy(pre, typeToShortString(_deviceMap->DeviceType));
  pre[2] = '0' + index;
  pre[3] = 0;

  if (SDNavTotalPrefixFiles.TotalItems(pre, _totalObjects))
  {
    // Find the index of the file 
    int i;
    u_int64_t size;

    strcpy(_currentObjectPath, "");
    
    for (i=0;i<_totalObjects;i++)
    {
      SDNavPrefixFileByIndex.GetFileByIndex(pre, i, g_tmpFilename, MAX_PATH_LEN, size);

      if (strcmp(_deviceMap->Filename, g_tmpFilename) == 0)
      {
        _currentObjectIndex = i;
        break;
      }
    }
  }
}

void BrowseScreenFlat::sdCardStateChange(bool cardIsPresent)
{
  _currntBrowseScreenType = -1;
}

void BrowseScreenFlat::init(int index)
{
  Screen::init(index);

  initScrollers(2);

  setupScroller(0, 42, 22, 88, 8, 1);
  setupScroller(1, 42, 36, 88, 8, 1);

  // If it's the first time into this screen
  bool needToInit = false;
  if (_scsiId != index || _currntBrowseScreenType != g_devices[index].BrowseScreenType)
  {
    needToInit = true;
  }

  _currntBrowseScreenType = _deviceMap->BrowseScreenType;
  _scsiId = index;
  _deviceMap = &g_devices[_scsiId];

  if (g_pendingLoadComplete > -1)
  {
    // We have return to the Browser from the MessageBox during an image load
    // so Patch the device now
    patchDevice(g_pendingLoadComplete);
    g_pendingLoadComplete = -1;
  }

  if (needToInit)
  {
    _currentObjectIndex = 0;

    switch(_deviceMap->BrowseMethod)
    {
      case BROWSE_METHOD_IMDDIR:
        initImgDir(index);
        break;

      case BROWSE_METHOD_IMGX:
        initImgX(index);
        break;

      case BROWSE_METHOD_USE_PREFIX:
        initPrefix(index);

      case BROWSE_METHOD_NOT_BROWSABLE:
        break;
    }
  }
  
  getCurrentFilenameAndUpdateScrollers();
}

void BrowseScreenFlat::draw()
{   
  _display->setCursor(0,0);             
  _display->print(F("Flat Browser"));

  _iconX = _display->width();
  
  DeviceMap &map = g_devices[_scsiId];

  _display->setTextSize(2);
  printNumberFromTheRight(_scsiId, 6, 0);
  _display->setTextSize(1);

  const uint8_t *deviceIcon = getIconForType((S2S_CFG_TYPE)map.DeviceType, true);
  drawIconFromRight(deviceIcon, 6, 0);

  _display->drawLine(0,10,_iconX+11,10, 1);

  _display->setCursor(0,36);             
  _display->print(F("Path: "));     

  _display->setCursor(0,22);             
  _display->print(F("Item: "));

  _display->setCursor(0,46);             
  _display->print(F("Size: "));
  _display->setCursor(42,46);    
    
  makeImageSizeStr(_currentObjectSize, _sizeBuffer);
  _display->print(_sizeBuffer);

  _display->setCursor(0,56);    
  _display->print(F("File"));
  
  _display->setCursor(42,56);    
  _display->print((_currentObjectIndex+1));
  _display->print(" of ");
  _display->print(_totalObjects);
}

void BrowseScreenFlat::shortRotaryPress()
{
  resetScrollerDelay();
}

void BrowseScreenFlat::shortUserPress()
{
  resetScrollerDelay();
  changeScreen(SCREEN_MAIN, SCREEN_ID_NO_PREVIOUS);
}

void BrowseScreenFlat::shortEjectPress()
{
  logmsg("void BrowseScreenFlat::shortEjectPress() start");
  resetScrollerDelay();
  loadSelectedImage();
  logmsg("void BrowseScreenFlat::shortEjectPress() end");
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
  switch(_deviceMap->BrowseMethod)
  {
    case BROWSE_METHOD_IMDDIR:
      if (_currentObjectIndex < _totalObjects) // Object from disc
      {
          if (g_cacheActive)
          {
            getCacheFile(_scsiId, _catChar, _currentObjectIndex, _currentObjectName, _currentObjectPath, _currentObjectSize);
          }
          else
          {
            SDNavFileByIndexRecursive.GetObjectByIndexRecursive(_deviceMap->RootFolder, _currentObjectIndex, _currentObjectName, _currentObjectPath, 64, _currentObjectSize);
          }
      }  
      break;

    case BROWSE_METHOD_IMGX:
      getImgXByIndex(_scsiId, _currentObjectIndex, _currentObjectName, MAX_PATH_LEN, _currentObjectSize);
      break;

    case BROWSE_METHOD_USE_PREFIX:
    {
      char pre[4];
      strcpy(pre, typeToShortString(_deviceMap->DeviceType));
      if (_scsiId >= 0 && _scsiId <= 9)
        pre[2] = '0' + _scsiId;
      else if (_scsiId >= 10  && _scsiId <= 15)
        pre[2] = 'A' + (_scsiId - 10);
      pre[3] = '\0';
      SDNavPrefixFileByIndex.GetFileByIndex(pre, _currentObjectIndex, _currentObjectName, MAX_PATH_LEN, _currentObjectSize);
    } 
    case BROWSE_METHOD_NOT_BROWSABLE:
      break;
  }
}

void BrowseScreenFlat::loadSelectedImage()
{
  switch(_deviceMap->BrowseMethod)
  {
    case BROWSE_METHOD_IMDDIR:
      strcpy(g_tmpFilepath, _currentObjectPath);
      strcat(g_tmpFilepath, "/");
      strcat(g_tmpFilepath, _currentObjectName);
      
      haltUIUpdates();
      if (loadImageDeferred(_scsiId, g_tmpFilepath, SCREEN_BROWSE_FLAT, _scsiId))
      {
        strcpy(_deviceMap->Path, _currentObjectPath);
        strcpy(_deviceMap->Filename, _currentObjectName);
      }
      break;

    case BROWSE_METHOD_IMGX:
      strcpy(g_tmpFilepath, _currentObjectName);

      haltUIUpdates();
      if (loadImageDeferred(_scsiId, g_tmpFilepath, SCREEN_BROWSE_FLAT, _scsiId))
      {
        strcpy(_deviceMap->Path, "");
        strcpy(_deviceMap->Filename, _currentObjectName);
      }
      break;

    case BROWSE_METHOD_USE_PREFIX:
    case BROWSE_METHOD_NOT_BROWSABLE:
      break;
  }
    
}

#endif