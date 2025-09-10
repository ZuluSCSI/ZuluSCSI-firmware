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

#include "control_global.h"
#include "SDNavigator.h"

TotalFilesSDNavigator SDNavTotalFiles;
ItemByIndexSDNavigator SDNavItemByIndex;
FindItemIndexByNameAndPathSDNavigator SDNavFindItemIndexByNameAndPath;
TotalPrefixFilesSDNavigator SDNavTotalPrefixFiles;
PrefixFileByIndexSDNavigator SDNavPrefixFileByIndex;
TotalFilesRecursiveSDNavigator SDNavTotalFilesRecursive;
FileByIndexRecursiveSDNavigator SDNavFileByIndexRecursive;
FindItemIndexByNameAndPathRecursiveSDNavigator SDNavFindItemIndexByNameAndPathRecursive;
ScanFilesRecursiveSDNavigator SDNavScanFilesRecursive;


// Categories
int g_totalCategories[S2S_MAX_TARGETS];
char g_categoryCodeAndNames[S2S_MAX_TARGETS][MAX_CATEGORIES][MAX_CATEGORY_NAME_LEN];

// Image Loading
char g_filenameToLoad[MAX_PATH_LEN];
int g_pendingLoadIndex = -1;
int g_pendingLoadComplete = -1;

#endif