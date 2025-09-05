#if defined(CONTROL_BOARD)

#ifndef CACHE_H
#define CACHE_H

extern bool g_cacheActive;

extern "C" bool doesDeviceHaveAnyCategoryFiles(int scsiId);

extern "C" void getCacheFile(int scsiId, char cat, int index, char *file, char *path, u_int64_t &size);
extern "C" int findCacheFile(int scsiId, char cat, char *file, char *path);

extern void clearCacheData();
extern void buildCache();

#endif

#endif