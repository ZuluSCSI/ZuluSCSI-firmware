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

#include "BrowseTypeScreen.h"
#include "ZuluSCSI_log.h"
#include "ZuluSCSI_disk.h"

#include "cache.h"
#include "control_global.h"

void BrowseTypeScreen::init(int index)
{
  ScreenList::init(index);

  _scsiId = index;
  _deviceMap = &g_devices[_scsiId];
  
  int totCats = g_totalCategories[_scsiId];
  _showFolder = _deviceMap->HasDirs;

  _totalItems = totCats + 1 + (_deviceMap->HasDirs ? 1 : 0);
}

void BrowseTypeScreen::draw()
{     
  _display->setCursor(0,0);             
  _display->print(F("Browse Type"));
  _display->drawLine(0,10,127,10, 1);

  ScreenList::draw();
}

void BrowseTypeScreen::shortUserPress()
{
  changeScreen(SCREEN_MAIN, SCREEN_ID_NO_PREVIOUS);
}

void BrowseTypeScreen::shortEjectPress()
{
  _deviceMap->BrowseScreenType = (SCREEN_BROWSETYPES)_selectedItem;

  if (_deviceMap->BrowseScreenType == SCREEN_BROWSETYPE_FOLDERS)
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
  _deviceMap->BrowseScreenType = (SCREEN_BROWSETYPES)_selectedItem;

  if (_deviceMap->BrowseScreenType == SCREEN_BROWSETYPE_FOLDERS)
  {
      changeScreen(SCREEN_BROWSE, _scsiId); 
  }
  else
  {
      changeScreen(SCREEN_BROWSE_FLAT, _scsiId); 
  }
}

void BrowseTypeScreen::drawItem(int x, int y, int index)
{ 
  if (index == 0 && _showFolder)
  {
    _display->setCursor(x+10, y+1);       
    _display->print("All (Folders)"); 
  }
  else if ((index == 1 && _showFolder) || (index == 0 && !_showFolder))
  {
    _display->setCursor(x+10, y+1);             
    _display->print("All (Flat)"); 
    _display->setCursor(x+110, y+1);      
    _display->print(_deviceMap->TotalFlatFiles); 
  }
  else // categories
  {
    int adj = _showFolder ? 2 : 1;
    char * catInfo = g_categoryCodeAndNames[_scsiId][index-adj];
    _display->setCursor(x+10, y+1);             
    _display->print("Cat: ");
    _display->print(&catInfo[1]); 
    _display->setCursor(x+110, y+1);          
    _display->print(_deviceMap->TotalFilesInCategory[index-adj]); 
  }

  ScreenList::drawItem(x, y, index);
}

#endif