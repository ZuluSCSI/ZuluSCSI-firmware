#if defined(CONTROL_BOARD) && !defined(ENABLE_AUDIO_OUTPUT_SPDIF)

#include "BrowseScreen.h"
#include "ZuluSCSI_log.h"
#include "ZuluSCSI_disk.h"
#include "MessageBox.h"
#include "control_global.h"

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
  int i;
  bool dir;
  u_int64_t size;
  for (i=0;i<_totalObjects;i++)
  {
    if (!findObjectByIndex(_scsiId, _currentObjectPath, i,  g_tmpFilename, (size_t)MAX_PATH_LEN, dir, size))
    {
      // error
      logmsg("Error searching for filename : ", _deviceMap->Filename);
      _currentObjectIndex = 0;
      break;
    }
    else
    {
      if (!dir)
      {
        if (strcmp(_deviceMap->Filename, g_tmpFilename) == 0)
        {
          _currentObjectIndex = i;
          break;
        }
      }
    }
  }

  // Need to recreate the stack
  // e.g. ISO3/folder  needs a stack of 1, where element 0 is the position of 'folder'
  bool done = false;
  char folderToCheck[MAX_PATH_LEN];
  char folderToLookFor[MAX_PATH_LEN];
  char tmpFilename[MAX_PATH_LEN];

  char *start = _currentObjectPath;
  while(!done)
  {
    char *firstSlash = strchr(start, (char)'/');
    
    u_int64_t size;
    bool isDir;

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
      // Here folderToCheck is the partial path
      logmsg("--- folderToCheck = '", folderToCheck, "'");
      logmsg("   --- folderToLookFor = '", folderToLookFor, "'");

      int totalObjects = totalObjectInDir(_scsiId, folderToCheck);
      for (i=0;i<totalObjects;i++)
      {
        if (!findObjectByIndex(_scsiId, folderToCheck, i,  tmpFilename, (size_t)MAX_PATH_LEN, isDir, size))
        {
          // error
          logmsg("Error searching for filename : ", _deviceMap->Filename);
          break;
        }
        else
        {
          if (isDir)
          {
            if (strcmp(folderToLookFor, tmpFilename) == 0)
            {
              _returnStack[_stackDepth++] = i;
              break;
            }
          }
        }
      }
    }
  } 
     
  getCurrentFilenameAndUpdateScrollers();
}

void BrowseScreen::draw()
{
  _display.setCursor(0,0);             
  _display.print(F("Browser"));

  _iconX = 92; // Inital offset from right (leave space for the SCSI ID)
  
  DeviceMap &map = g_devices[_scsiId];

  const uint8_t *deviceIcon = getIconForType((S2S_CFG_TYPE)map.DeviceType, true);
  drawIconFromRight(deviceIcon, 6, 0);

  _display.drawLine(0,10,_iconX+11,10, 1);

  _display.setTextSize(2);             
  _display.setCursor(112,0);       
  _display.print(_scsiId);           
  _display.setTextSize(1);            

  _display.setCursor(0,36);             
  _display.print(F("Path: "));     

  _display.setCursor(0,22);             
  _display.print(F("Item: "));

  if (!_isCurrentObjectADir)
  {
    _display.setCursor(0,46);             
    _display.print(F("Size: "));
    _display.setCursor(42,46);    

    makeImageSizeStr(_currentObjectSize, _sizeBuffer);
    _display.print(_sizeBuffer);
  }

  _display.setCursor(0,56);    
  if (_isCurrentObjectADir)
  {
    _display.print(F("Dir"));
  }
  else
  {
    _display.print(F("File"));
  }

  _display.setCursor(42,56);    
  _display.print((_currentObjectIndex+1));
  _display.print(" of ");
  _display.print(totalNavigationObjects());
}

void BrowseScreen::sdCardStateChange(bool cardIsPresent)
{
  _stackDepth = 0;
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
    changeScreen(SCREEN_MAIN, -1);
  }
}

void BrowseScreen::shortEjectPress()
{
  if (_isCurrentObjectADir)
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
  setScrollerText(0, _currentObjectName);
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
        bool isDir;
        u_int64_t size;
        
        if (!findObjectByIndex(_scsiId, _currentObjectPath, index, g_tmpFilename, (size_t)MAX_PATH_LEN, isDir, size))
        {
            // Error
        }

        if (isDir)
        {
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
      _isCurrentObjectADir = true; 

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
  _totalObjects = totalObjectInDir(_scsiId, _currentObjectPath);
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
        if (!findObjectByIndex(_scsiId, _currentObjectPath, _currentObjectIndex,  _currentObjectName, (size_t)MAX_PATH_LEN, _isCurrentObjectADir, _currentObjectSize))
        {
            logmsg("* ERROR - BrowseScreen::getCurrentFilename(). Couldnt get FindObjectByIndex: ", (int)_scsiId);
        }
    }
    else if (!isCurrentFolderRoot() && _currentObjectIndex ==  (totalObjects-1))
    {
        strcpy(_currentObjectName, _back);
        _isCurrentObjectADir = true; 
    }
}

bool BrowseScreen::isCurrentFolderRoot()
{
    return strcmp(_currentObjectPath, _deviceMap->RootFolder) == 0;
}

void BrowseScreen::loadSelectedImage()
{
  if (!_isCurrentObjectADir)
  {
      strcpy(g_tmpFilepath, _currentObjectPath);
      strcat(g_tmpFilepath, "/");
      strcat(g_tmpFilepath, _currentObjectName);
      
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