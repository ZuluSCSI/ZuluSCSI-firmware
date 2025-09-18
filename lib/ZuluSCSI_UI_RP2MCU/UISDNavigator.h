#ifndef UISDNAVIGATOR_H
#define UISDNAVIGATOR_H

#include "ZuluSCSI_disk.h"
#include "SDNavigator.h"

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
    bool GetObjectByIndex(const char *dirname, int index, char* filename, size_t buflen, bool &isDir, u_int64_t &size);

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

extern TotalFilesSDNavigator SDNavTotalFiles;   // This works the safe for both RAW nd bincue remmaping

extern ItemByIndexSDNavigator SDNavItemByIndex;   // TODO BINCUE remap
extern FindItemIndexByNameAndPathSDNavigator SDNavFindItemIndexByNameAndPath;  // TODO BINCUE remap
extern FileByIndexRecursiveSDNavigator SDNavFileByIndexRecursive;  // TODO BINCUE remap
extern FindItemIndexByNameAndPathRecursiveSDNavigator SDNavFindItemIndexByNameAndPathRecursive;  // TODO BINCUE remap
extern ScanFilesRecursiveSDNavigator SDNavScanFilesRecursive;  // TODO BINCUE remap

// no remapping for bin/cue
extern TotalPrefixFilesSDNavigator SDNavTotalPrefixFiles;
extern PrefixFileByIndexSDNavigator SDNavPrefixFileByIndex;


#endif
