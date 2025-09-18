#ifndef SDNAVIGATOR_H
#define SDNAVIGATOR_H

#include "ZuluSCSI_disk.h"
#include "ui.h"

extern char g_tmpFilename[MAX_PATH_LEN];
extern char g_tmpFilepath[MAX_PATH_LEN];

typedef enum
{
    PROCESS_DIR_ITEM_RESULT_PROCEED,
    PROCESS_DIR_ITEM_RESULT_STOP_PROCESSING,
    // PROCESS_DIR_ITEM_RESULT_ERROR
} PROCESS_DIR_ITEM_RESULT;

typedef enum
{
    WALK_DIR_ITEM_RESULT_OK,
    WALK_DIR_ITEM_RESULT_FAIL,
    WALK_DIR_ITEM_RESULT_STOP_PROCESSING,
} WALK_DIR_RESULT;


class SDNavigator
{
protected:	
    WALK_DIR_RESULT WalkDirectory(const char* dirname, bool recursive, bool includeAllFiles);

	virtual PROCESS_DIR_ITEM_RESULT ProcessDirectoryItem(FsFile &file, const char *filename, const char *path); // returning true mean stop processing

    bool startsWith(const char* str, const char* prefix);

    bool _hasSubDirs;
};

class FindItemIndexByNameAndPathSDNavigatorRaw : public SDNavigator
{
public:
    int FindItemByNameAndPath(const char *dirname, const char *filename, bool &isDir);

protected:	
	PROCESS_DIR_ITEM_RESULT ProcessDirectoryItem(FsFile &file, const char *filename, const char *path);

    bool _isDir;
    int _counter;
    const char *_filename;
};


class ScanFilesSDNavigatorRaw : public SDNavigator
{
public:
    bool ScanFiles(const char *dirname, void (*callback)(int, const char *, const char *, u_int64_t));

protected:	
	PROCESS_DIR_ITEM_RESULT ProcessDirectoryItem(FsFile &file, const char *filename, const char *path);

    void (*_callback)(int, const char *, const char *, u_int64_t);

    int _counter;
};


class GetFirstFileRecursiveSDNavigator : public SDNavigator
{
public:
    bool GetFirstFileRecursive(const char *dirname, char *filename, char *path);

protected:	
	PROCESS_DIR_ITEM_RESULT ProcessDirectoryItem(FsFile &file, const char *filename, const char *path);

    const char *_filename;
    const char *_path;
};

extern GetFirstFileRecursiveSDNavigator SDNavGetFirstFileRecursive;

#endif
