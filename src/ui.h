#ifndef UI_H
#define UI_H

#define TOTAL_DEVICES 8
#define MAX_PATH_LEN 64
#define MAX_CATEGORIES 10
#define MAX_CATEGORY_NAME_LEN 32

extern "C" void scsiReinitComplete();
extern "C" void sdCardStateChanged(bool absent);

extern "C" void controlInit();
extern "C" void controlLoop();
extern "C" void loadImage();      // in ZuluSCSI_disk used in ZuluSCSI

extern "C" void setFolder(int target_idx, bool userSet, const char *path);

extern "C" void initUI();

extern int g_pendingLoadIndex;
extern int g_totalCategories[TOTAL_DEVICES];
extern char g_categoryCodeAndNames[TOTAL_DEVICES][MAX_CATEGORIES][MAX_CATEGORY_NAME_LEN];
extern int g_pendingLoadComplete;
extern char g_filenameToLoad[MAX_PATH_LEN];



#endif
