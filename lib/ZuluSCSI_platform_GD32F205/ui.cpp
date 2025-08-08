#include "..\src\ui.h"

extern "C" void scsiReinitComplete()
{

}
extern "C" void sdCardStateChanged(bool absent)
{

}

extern "C" void controlInit()
{

}
extern "C" void controlLoop()
{

}


extern "C" void setFolder(int target_idx, bool userSet, const char *path)
{

}

extern "C" void initUI()
{

}

int g_pendingLoadIndex;
int g_totalCategories[8];
char g_categoryCodeAndNames[8][10][32];
char g_filenameToLoad[MAX_PATH_LEN];
int g_pendingLoadComplete;