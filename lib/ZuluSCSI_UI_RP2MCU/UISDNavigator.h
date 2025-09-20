#ifndef UISDNAVIGATOR_H
#define UISDNAVIGATOR_H

#include "ZuluSCSI_disk.h"
#include "SDNavigator.h"

class TotalFilesSDNavigator : public SDNavigator
{
public:
    bool TotalItems(const char* dirname, int &total);

protected:	
	PROCESS_DIR_ITEM_RESULT ProcessDirectoryItem(const char *filename, const char *path, u_int64_t size, NAV_OBJECT_TYPE navObjectType, char *cueFilename);

    int _total;
};

class ItemByIndexSDNavigator : public SDNavigator
{
public:
    bool GetObjectByIndex(const char *dirname, int index, char* filename, size_t buflen, NAV_OBJECT_TYPE &navObjectType, u_int64_t &size, char *cueFilename);

protected:	
	PROCESS_DIR_ITEM_RESULT ProcessDirectoryItem(const char *filename, const char *path, u_int64_t size, NAV_OBJECT_TYPE navObjectType, char *cueFilename);

    int _index;
    int _counter;

    NAV_OBJECT_TYPE _navObjectType;
    u_int64_t _size;
    const char *_filename;
    const char *_cueFilename;
};

class FindItemIndexByNameAndPathSDNavigator : public SDNavigator
{
public:
    int FindItemByNameAndPath(const char *dirname, const char *filename, NAV_OBJECT_TYPE &navObjectType);

protected:	
	PROCESS_DIR_ITEM_RESULT ProcessDirectoryItem(const char *filename, const char *path, u_int64_t size, NAV_OBJECT_TYPE navObjectType, char *cueFilename);

    NAV_OBJECT_TYPE _navObjectType;
    int _counter;
    const char *_filename;
};

class TotalPrefixFilesSDNavigator : public SDNavigator
{
public:
    bool TotalItems(const char* prefix, int &total);

protected:	
	PROCESS_DIR_ITEM_RESULT ProcessDirectoryItem(const char *filename, const char *path, u_int64_t size, NAV_OBJECT_TYPE navObjectType, char *cueFilename);

    const char* _prefix;
    int _total;
};

class PrefixFileByIndexSDNavigator : public SDNavigator
{
public:
    bool GetFileByIndex(const char *prefix, int index, char* buf, size_t buflen, u_int64_t &size);

protected:	
	PROCESS_DIR_ITEM_RESULT ProcessDirectoryItem(const char *filename, const char *path, u_int64_t size, NAV_OBJECT_TYPE navObjectTyper, char *cueFilename);

    int _index;
    int _counter;

    const char* _prefix;

    u_int64_t _size;
    const char *_filename;
};


class FileByIndexRecursiveSDNavigator : public SDNavigator
{
public:
    bool GetObjectByIndexRecursive(const char *dirname, int index, char* filename, char *path, size_t buflen, u_int64_t &size, NAV_OBJECT_TYPE &navObjectType, char *cueFilename);

protected:	
	PROCESS_DIR_ITEM_RESULT ProcessDirectoryItem(const char *filename, const char *path, u_int64_t size, NAV_OBJECT_TYPE navObjectType, char *cueFilename);

    int _index;
    int _counter;

    u_int64_t _size;
    const char *_filename;
    NAV_OBJECT_TYPE _navObjectType;
    const char *_cueFilename;
    const char *_path;
};

class FindItemIndexByNameAndPathRecursiveSDNavigator : public SDNavigator
{
public:
    int FindItemIndexByNameAndPathRecursive(const char *dirname, char* filename, const char *path);

protected:	
	PROCESS_DIR_ITEM_RESULT ProcessDirectoryItem(const char *filename, const char *path, u_int64_t size, NAV_OBJECT_TYPE navObjectType, char *cueFilename);

    int _counter;

    const char *_filename;
    const char *_path;
};



class ScanFilesRecursiveSDNavigator : public SDNavigator
{
public:
    bool ScanFilesRecursive(const char *dirname, bool &hasDirs, void (*callback)(int, const char *, const char *, u_int64_t, NAV_OBJECT_TYPE, const char *));

protected:	
	PROCESS_DIR_ITEM_RESULT ProcessDirectoryItem(const char *filename, const char *path, u_int64_t size, NAV_OBJECT_TYPE navObjectType, char *cueFilename);

    void (*_callback)(int, const char *, const char *, u_int64_t, NAV_OBJECT_TYPE, const char *);

    int _counter;
    bool _hasDirs;
};

extern TotalFilesSDNavigator SDNavTotalFiles;  

extern ItemByIndexSDNavigator SDNavItemByIndex;  
extern FindItemIndexByNameAndPathSDNavigator SDNavFindItemIndexByNameAndPath;  
extern FileByIndexRecursiveSDNavigator SDNavFileByIndexRecursive; 
extern FindItemIndexByNameAndPathRecursiveSDNavigator SDNavFindItemIndexByNameAndPathRecursive; 
extern ScanFilesRecursiveSDNavigator SDNavScanFilesRecursive;  

// no remapping for bin/cue
extern TotalPrefixFilesSDNavigator SDNavTotalPrefixFiles;
extern PrefixFileByIndexSDNavigator SDNavPrefixFileByIndex;


#endif
