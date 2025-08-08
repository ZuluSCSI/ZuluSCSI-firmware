#ifndef UI_H
#define UI_H

#define MAX_PATH_LEN 260
#define MAX_CATEGORIES 10

extern "C" void scsiReinitComplete();
extern "C" void sdCardStateChanged(bool absent);

extern "C" void controlInit();
extern "C" void controlLoop();
extern "C" void loadImage();      // in ZuluSCSI_disk used in ZuluSCSI

extern "C" void setFolder(int target_idx, bool userSet, const char *path);

extern "C" void initUI();

extern int g_pendingLoadIndex;
extern int g_totalCategories[8];
extern char g_categoryCodeAndNames[8][10][32];
extern int g_pendingLoadComplete;
extern char g_filenameToLoad[MAX_PATH_LEN];



#endif
