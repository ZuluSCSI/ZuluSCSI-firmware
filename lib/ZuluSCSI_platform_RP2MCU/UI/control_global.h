#if defined(CONTROL_BOARD) && !defined(ENABLE_AUDIO_OUTPUT_SPDIF)

#ifndef CONTROL_GLOBAL_H
#define CONTROL_GLOBAL_H

#include "ui.h"
#include "ZuluSCSI_disk.h"

// new in ZuluSCSI_disk.h
extern image_config_t g_DiskImages[S2S_MAX_TARGETS];

extern "C" void setPendingImageLoad(uint8_t id, const char* next_filename);

extern "C" int totalObjectInDir(uint8_t id, const char *dirname);
extern "C" int findObjectByIndex(uint8_t id, const char *dirname, int index, char* buf, size_t buflen, bool &isDir, u_int64_t &size);

extern "C" int totalFilesRecursiveInDir(uint8_t id, const char *dirname); 
extern "C" int findFilesecursiveByIndex(uint8_t id, const char *dirname, int index, char* buf, char *path, size_t buflen, u_int64_t &size);
extern "C" int scanFilesRecursiveInDir(uint8_t id, const char *dirname, bool &hasDirs, void (*callback)(int, char *, char *, u_int64_t));
extern "C" bool getFirstFile(uint8_t id, const char *dirname, char *file);
extern "C" void getImgXByIndex(uint8_t id, int index, char* buf, size_t buflen, u_int64_t &size);

extern "C" int totalPrefixObjects(const char* prefix);
extern "C" int findPrefixObjectByIndex(const char* prefix, uint32_t index, char* buf, size_t buflen, u_int64_t &size);

// new in ZuluSCSI_log.h
extern const char *g_log_short_firmwareversion;

// new in ZuluSCSI_platform.h
extern bool g_scsi_initiator;



#endif

#endif