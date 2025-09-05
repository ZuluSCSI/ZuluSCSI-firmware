#if defined(CONTROL_BOARD) && !defined(ENABLE_AUDIO_OUTPUT_SPDIF)

#include "control_global.h"
#include "SDNavigator.h"

TotalFilesSDNavigator SDNavTotalFiles;
ItemByIndexSDNavigator SDNavItemByIndex;
FindItemIndexByNameAndPathSDNavigator SDNavFindItemIndexByNameAndPath;
TotalPrefixFilesSDNavigator SDNavTotalPrefixFiles;
PrefixFileByIndexSDNavigator SDNavPrefixFileByIndex;
TotalFilesRecursiveSDNavigator SDNavTotalFilesRecursive;
FileByIndexRecursiveSDNavigator SDNavFileByIndexRecursive;
ScanFilesRecursiveSDNavigator SDNavScanFilesRecursive;


// Categories
int g_totalCategories[8];
char g_categoryCodeAndNames[8][10][32];

// Image Loading
char g_filenameToLoad[MAX_PATH_LEN];
int g_pendingLoadIndex = -1;
int g_pendingLoadComplete = -1;

#endif