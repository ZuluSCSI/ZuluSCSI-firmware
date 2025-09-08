#if defined(CONTROL_BOARD)

#ifndef CONTROL_GLOBAL_H
#define CONTROL_GLOBAL_H

#include "ui.h"
#include "ZuluSCSI_disk.h"
#include "SDNavigator.h"

// new in ZuluSCSI_disk.h
extern image_config_t g_DiskImages[S2S_MAX_TARGETS];

extern "C" void setPendingImageLoad(uint8_t id, const char* next_filename);
extern "C" void getImgXByIndex(uint8_t id, int index, char* buf, size_t buflen, u_int64_t &size);

// new in ZuluSCSI_log.h
extern const char *g_log_short_firmwareversion;

// new in ZuluSCSI_platform.h
extern bool g_scsi_initiator;

extern TotalFilesSDNavigator SDNavTotalFiles;
extern ItemByIndexSDNavigator SDNavItemByIndex;
extern FindItemIndexByNameAndPathSDNavigator SDNavFindItemIndexByNameAndPath;
extern TotalPrefixFilesSDNavigator SDNavTotalPrefixFiles;
extern PrefixFileByIndexSDNavigator SDNavPrefixFileByIndex;
extern TotalFilesRecursiveSDNavigator SDNavTotalFilesRecursive;
extern FileByIndexRecursiveSDNavigator SDNavFileByIndexRecursive;
extern FindItemIndexByNameAndPathRecursiveSDNavigator SDNavFindItemIndexByNameAndPathRecursive;
extern ScanFilesRecursiveSDNavigator SDNavScanFilesRecursive;

#endif

#endif