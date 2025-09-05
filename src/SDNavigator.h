#ifndef SDNAVIGATOR_H
#define SDNAVIGATOR_H

#include "ZuluSCSI_disk.h"

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
    WALK_DIR_RESULT WalkDirectory(const char* dirname, bool recursive);

	virtual PROCESS_DIR_ITEM_RESULT ProcessDirectoryItem(FsFile &file, const char *filename, const char *path); // returning true mean stop processing

    bool startsWith(const char* str, const char* prefix);

    bool _hasSubDirs;
};

class TotalFilesSDNavigator : public SDNavigator
{
public:
    bool TotalItems(const char* dirname, int &total);

protected:	
	PROCESS_DIR_ITEM_RESULT ProcessDirectoryItem(FsFile &file, const char *filename, const char *path);

    int _total;
};

class ItemByIndexSDNavigator : public SDNavigator
{
public:
    bool GetObjectByIndex(const char *dirname, int index, char* buf, size_t buflen, bool &isDir, u_int64_t &size);

protected:	
	PROCESS_DIR_ITEM_RESULT ProcessDirectoryItem(FsFile &file, const char *filename, const char *path);

    int _index;
    int _counter;

    bool _isDir;
    u_int64_t _size;
    const char *_filename;
};

class FindItemIndexByNameAndPathSDNavigator : public SDNavigator
{
public:
    int FindItemByNameAndPath(const char *dirname, const char *filename, bool &isDir);

protected:	
	PROCESS_DIR_ITEM_RESULT ProcessDirectoryItem(FsFile &file, const char *filename, const char *path);

    bool _isDir;
    int _counter;
    const char *_filename;
};

class TotalPrefixFilesSDNavigator : public SDNavigator
{
public:
    bool TotalItems(const char* prefix, int &total);

protected:	
	PROCESS_DIR_ITEM_RESULT ProcessDirectoryItem(FsFile &file, const char *filename, const char *path);

    const char* _prefix;
    int _total;
};

class PrefixFileByIndexSDNavigator : public SDNavigator
{
public:
    bool GetFileByIndex(const char *prefix, int index, char* buf, size_t buflen, u_int64_t &size);

protected:	
	PROCESS_DIR_ITEM_RESULT ProcessDirectoryItem(FsFile &file, const char *filename, const char *path);

    int _index;
    int _counter;

    const char* _prefix;

    u_int64_t _size;
    const char *_filename;
};

class TotalFilesRecursiveSDNavigator : public SDNavigator
{
public:
    bool TotalItemsRecursive(const char* dirname, int &total);

protected:	
	PROCESS_DIR_ITEM_RESULT ProcessDirectoryItem(FsFile &file, const char *filename, const char *path);

    int _total;
};

class FileByIndexRecursiveSDNavigator : public SDNavigator
{
public:
    bool GetObjectByIndexRecursive(const char *dirname, int index, char* filename, char *path, size_t buflen, u_int64_t &size);

protected:	
	PROCESS_DIR_ITEM_RESULT ProcessDirectoryItem(FsFile &file, const char *filename, const char *path);

    int _index;
    int _counter;

    u_int64_t _size;
    const char *_filename;
    const char *_path;
};

class FindItemIndexByNameAndPathRecursiveSDNavigator : public SDNavigator
{
public:
    int FindItemIndexByNameAndPathRecursive(const char *dirname, char* filename, const char *path);

protected:	
	PROCESS_DIR_ITEM_RESULT ProcessDirectoryItem(FsFile &file, const char *filename, const char *path);

    int _counter;

    const char *_filename;
    const char *_path;
};


class ScanFilesRecursiveSDNavigator : public SDNavigator
{
public:
    bool ScanFilesRecursive(const char *dirname, bool &hasDirs, void (*callback)(int, const char *, const char *, u_int64_t));

protected:	
	PROCESS_DIR_ITEM_RESULT ProcessDirectoryItem(FsFile &file, const char *filename, const char *path);

    void (*_callback)(int, const char *, const char *, u_int64_t);

    int _counter;
    bool _hasDirs;
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

#endif
