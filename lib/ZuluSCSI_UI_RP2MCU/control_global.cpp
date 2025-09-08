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