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

#include "BrowseScreen.h"
#include "ZuluSCSI_log.h"
#include "ZuluSCSI_disk.h"
#include "MessageBox.h"
#include "control_global.h"
#include "UISDNavigator.h"
#include "ZuluSCSI_settings.h"

void BrowseScreen::init(int index)
{
  Screen::init(index);

  initScrollers(2);

  setupScroller(0, 42, 22, 88, 8, 1);
  setupScroller(1, 42, 36, 88, 8, 1);

  _scsiId = index;
  _deviceMap = &g_devices[_scsiId];

  if (g_pendingLoadComplete > -1)
  {
    // We have return to the Browser from the MessageBox during an image load
    // so Patch the device now
    patchDevice(g_pendingLoadComplete);
    g_pendingLoadComplete = -1;
  }

  setFolderAndUpdateScrollers(_deviceMap->Path, 0 );

  // Find the index of the file 
  NAV_OBJECT_TYPE navObjectType;
  _currentObjectIndex = SDNavFindItemIndexByNameAndPath.FindItemByNameAndPath(_currentObjectPath, _deviceMap->LoadedObject, navObjectType);
  if (_currentObjectIndex == -1)
  {
    _currentObjectIndex = 0;
  }
  
  // Need to recreate the stack
  // e.g. ISO3/folder  needs a stack of 1, where element 0 is the position of 'folder'
  bool done = false;
  char folderToCheck[MAX_PATH_LEN];
  char folderToLookFor[MAX_PATH_LEN];
  
  char *start = _currentObjectPath;
  while(!done)
  {
    char *firstSlash = strchr(start, (char)'/');
    
    NAV_OBJECT_TYPE navObjectType;

    if (firstSlash == NULL) // End of the list
    {
        done = true;
    }
    else
    {
        int len = (firstSlash - _currentObjectPath);
        strncpy(folderToCheck, _currentObjectPath, ((size_t)len));
        folderToCheck[len] = 0;
        start = firstSlash+1;

        char *secondSlash = strchr(start, (char)'/');
        if (secondSlash == NULL)
        {
          strcpy(folderToLookFor, firstSlash+1);
        }
        else
        {
          int len = (secondSlash - firstSlash)-1;
          strncpy(folderToLookFor, firstSlash+1, ((size_t)len));
          folderToLookFor[len] = 0;
        }
    }

    if (!done)
    {
      // logmsg("--- folderToCheck = '", folderToCheck, "'");
      // logmsg("   --- folderToLookFor = '", folderToLookFor, "'");

      int index = SDNavFindItemIndexByNameAndPath.FindItemByNameAndPath(folderToCheck, folderToLookFor, navObjectType);
      if (navObjectType == NAV_OBJECT_DIR)
      {
        _returnStack[_stackDepth++] = index;
      }  
    }
  } 
     
  getCurrentFilenameAndUpdateScrollers();
}

void BrowseScreen::draw()
{
  _display->setCursor(0,0);             
  _display->print(F("Browser"));

   _iconX = _display->width();

  DeviceMap &map = g_devices[_scsiId];

  _display->setTextSize(2);
  printNumberFromTheRight(_scsiId, 6, 0);
  _display->setTextSize(1);

  const uint8_t *deviceIcon = getIconForType(map.DeviceType, true);
  drawIconFromRight(deviceIcon, 6, 0);

  _display->drawLine(0,10,_iconX+11,10, 1);

  _display->setCursor(0,22);             
  _display->print(F("Item: "));

  _display->setCursor(0,36);
  _display->print(F("Path: "));     

  if (_currentObjectType != NAV_OBJECT_DIR)
  {
    _display->setCursor(0,46);             
    _display->print(F("Size: "));
    _display->setCursor(42,46);    

    makeImageSizeStr(_currentObjectSize, _sizeBuffer);
    _display->print(_sizeBuffer);
  }

  _display->setCursor(0,56);    
  switch(_currentObjectType)
  {
    case NAV_OBJECT_FILE:
      _display->print(F("File"));
      break;
    case NAV_OBJECT_DIR:
      _display->print(F("Dir"));
      break;
    case NAV_OBJECT_CUE:
      _display->print(F("BinCue"));
      break;
  }
  
  _display->setCursor(42,56);    
  _display->print((_currentObjectIndex+1));
  _display->print(" of ");
  _display->print(totalNavigationObjects());
}

void BrowseScreen::sdCardStateChange(bool cardIsPresent)
{
  _stackDepth = 0;
  strcpy(_deviceMap->LoadedObject, "");
}

void BrowseScreen::shortRotaryPress()
{
  resetScrollerDelay();
  selectCurrentObject();
}

void BrowseScreen::longRotaryPress()
{
  if (goBackADirectory())
  {
    resetScrollerDelay();
  }
}

void BrowseScreen::shortUserPress()
{
  if (!goBackADirectory())
  {
    resetScrollerDelay();
    changeScreen(SCREEN_MAIN, SCREEN_ID_NO_PREVIOUS);
  }
}

void BrowseScreen::shortEjectPress()
{
  if (_currentObjectType == NAV_OBJECT_DIR)
  {
    selectCurrentObject();
  }
  else
  {
    resetScrollerDelay();
    loadSelectedImage();
  }
}

void BrowseScreen::rotaryChange(int direction)
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

void BrowseScreen::getCurrentFilenameAndUpdateScrollers()
{
  getCurrentFilename();
  setScrollerText(0, _currentObjectDisplayName);
}

void BrowseScreen::setFolderAndUpdateScrollers(char *folder, int initialFile)
{
  setFolder(folder, initialFile);
  setScrollerText(1, _currentObjectPath);
}

int BrowseScreen::navigateToNextObject()
{
    int totalObjects = totalNavigationObjects();
    _currentObjectIndex++;
    if (_currentObjectIndex == totalObjects)
    {
        _currentObjectIndex = 0;
    }

    getCurrentFilenameAndUpdateScrollers();
    forceDraw();

    return 1;
}

int BrowseScreen::navigateToPreviousObject()
{
    int totalObjects = totalNavigationObjects();
    _currentObjectIndex--;
    if (_currentObjectIndex == -1)
    {
        _currentObjectIndex = totalObjects-1;
    }

    getCurrentFilenameAndUpdateScrollers();
    forceDraw();

    return 1;
}

int BrowseScreen::selectCurrentObject()
{
    int index = _currentObjectIndex;

    if (index < _totalObjects) // Object from disc
    {
      NAV_OBJECT_TYPE navObjectType;
      u_int64_t size;
      char tmp[MAX_FILE_PATH];

      if (!SDNavItemByIndex.GetObjectByIndex(_currentObjectPath, index, g_tmpFilename, (size_t)MAX_PATH_LEN, navObjectType, size, tmp))
      {
          return 0;
      }
      else
      {
        if (navObjectType == NAV_OBJECT_DIR)
        {
            int newPathLen = strlen(_currentObjectPath) + strlen(g_tmpFilename) + 1;
            if ( newPathLen > (MAX_PATH_LEN-1))
            {
                _messageBox->setReturnScreen(SCREEN_BROWSE);
                _messageBox->setText("-- Warning --", "Filepath", "too long...");
                changeScreen(MESSAGE_BOX, _scsiId);
                return 0;
            }

            // About to go into a dir, so save the position in the list at this level for when we return.
            _returnStack[_stackDepth++] = _currentObjectIndex;
  
            strcpy(g_tmpFilepath, _currentObjectPath);
            strcat(g_tmpFilepath, "/");
            strcat(g_tmpFilepath, g_tmpFilename);

            setFolderAndUpdateScrollers(g_tmpFilepath, 0);

            getCurrentFilenameAndUpdateScrollers();
        }
        else
        {          
            // This is a file click. which doesn't nothing.   
        }
      }
    }
    else
    {
        char *slash = strrchr(_currentObjectPath, (char)'/');
        if (slash != NULL)
        {
            *slash = 0;
        }

        _stackDepth--;
        setFolderAndUpdateScrollers(_currentObjectPath, _returnStack[_stackDepth]);
        getCurrentFilenameAndUpdateScrollers();
    }

    forceDraw();
    return 1;
}

// Backend

bool BrowseScreen::goBackADirectory()
{
  if (!isCurrentFolderRoot())
  {
      strcpy(_currentObjectName, _back);
      strcpy(_currentObjectDisplayName, _currentObjectName);
      _currentObjectType = NAV_OBJECT_DIR;

      _currentObjectIndex = _totalObjects;
      selectCurrentObject();

      return true;
  }
  return false;
}

void BrowseScreen::setFolder(char *folder, int initialFile)
{
  strcpy(_currentObjectPath, folder);
  _currentObjectIndex = initialFile;
  SDNavTotalFiles.TotalItems(_currentObjectPath, _totalObjects);
}

int BrowseScreen::totalNavigationObjects()
{
    return _totalObjects + (isCurrentFolderRoot()?0:1);
}

void BrowseScreen::getCurrentFilename()
{
    int totalObjects = totalNavigationObjects();

    if (_currentObjectIndex < _totalObjects) // Object from disc
    {
        if (!SDNavItemByIndex.GetObjectByIndex(_currentObjectPath, _currentObjectIndex,  _currentObjectName, (size_t)MAX_PATH_LEN, _currentObjectType, _currentObjectSize, _currentObjectDisplayName))
        {
            logmsg("* ERROR - BrowseScreen::getCurrentFilename(). Couldnt get FindObjectByIndex: ", (int)_scsiId);
        }

        // The assumption above is that _currentObjectDisplayName is a cue, but if it's not, copy the objectName
        scsi_system_settings_t *cfg = g_scsi_settings.getSystem();
        if (_currentObjectType != NAV_OBJECT_CUE || !cfg->controlBoardShowCueFileName) 
        {
          strcpy(_currentObjectDisplayName, _currentObjectName);
        }
    }
    else if (!isCurrentFolderRoot() && _currentObjectIndex ==  (totalObjects-1))
    {
        strcpy(_currentObjectName, _back);
        strcpy(_currentObjectDisplayName, _currentObjectName);
        _currentObjectType = NAV_OBJECT_DIR; 
    }
}

bool BrowseScreen::isCurrentFolderRoot()
{
    return strcmp(_currentObjectPath, _deviceMap->RootFolder) == 0;
}

void BrowseScreen::loadSelectedImage()
{
  if (_currentObjectType != NAV_OBJECT_DIR)
  {
      strcpy(g_tmpFilepath, _currentObjectPath);
      strcat(g_tmpFilepath, "/");
      strcat(g_tmpFilepath, _currentObjectName);

      strcpy(_deviceMap->LoadedObject, _currentObjectName);
      _deviceMap->NavObjectType  = _currentObjectType;

      logmsg("Loading Image file: ", g_tmpFilepath);
      
      haltUIUpdates();
      if (loadImageDeferred(_scsiId, g_tmpFilepath, SCREEN_BROWSE, _scsiId))
      {
        strcpy(_deviceMap->Filename, _currentObjectName);
        strcpy(_deviceMap->Path, _currentObjectPath);
      }
  }
}

// Debug

void BrowseScreen::printStack()
{
  int i;
  logmsg("* STACK: size ", _stackDepth);
  for (i=0;i<_stackDepth;i++)
  {
    logmsg("  Item: ", i, "  val = ", _returnStack[i]);
  }
}

#endif