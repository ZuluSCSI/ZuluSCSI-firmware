#include "UISDNavigator.h"
#include "ZuluSCSI_log.h"
#include "ui.h"
#include <ctype.h>
#include "control.h"

TotalFilesSDNavigator SDNavTotalFiles;
ItemByIndexSDNavigator SDNavItemByIndex;
FindItemIndexByNameAndPathSDNavigator SDNavFindItemIndexByNameAndPath;
TotalPrefixFilesSDNavigator SDNavTotalPrefixFiles;
PrefixFileByIndexSDNavigator SDNavPrefixFileByIndex;
FileByIndexRecursiveSDNavigator SDNavFileByIndexRecursive;
FindItemIndexByNameAndPathRecursiveSDNavigator SDNavFindItemIndexByNameAndPathRecursive;
ScanFilesRecursiveSDNavigator SDNavScanFilesRecursive;

extern char g_tmpFilename[MAX_PATH_LEN];
extern char g_tmpFilepath[MAX_PATH_LEN];

char navTmp[MAX_PATH_LEN];
char navTmpTest[MAX_PATH_LEN];

// TotalFilesSDNavigator
////////////////////////
PROCESS_DIR_ITEM_RESULT TotalFilesSDNavigator::ProcessDirectoryItem(const char *filename, const char *path, u_int64_t size, NAV_OBJECT_TYPE navObjectType, char *cueFilename)
{
    _total++;
    return PROCESS_DIR_ITEM_RESULT_PROCEED;
}

bool TotalFilesSDNavigator::TotalItems(const char* dirname, int &total)
{
    _total = 0;
    _hasSubDirs = false;

    if (WalkDirectory(dirname, false, false, false) != WALK_DIR_ITEM_RESULT_FAIL)
    {
        total =  _total;
        return true;
    }

    total = -1;
    return false;
}

// ItemByIndexSDNavigator
/////////////////////////
PROCESS_DIR_ITEM_RESULT ItemByIndexSDNavigator::ProcessDirectoryItem(const char *filename, const char *path, u_int64_t size, NAV_OBJECT_TYPE navObjectType, char *cueFilename)
{
    if (_counter == _index)
    {
        _navObjectType = navObjectType;
        _size = size;
        _filename = filename;
        _cueFilename = cueFilename;
        
        return PROCESS_DIR_ITEM_RESULT_STOP_PROCESSING;    
    }

    _counter++;
    return PROCESS_DIR_ITEM_RESULT_PROCEED;
}

bool ItemByIndexSDNavigator::GetObjectByIndex(const char *dirname, int index, char* filename, size_t buflen, NAV_OBJECT_TYPE &navObjectType, u_int64_t &size, char *cueFilename)
{
    _counter = 0;
    _index = index;
    _hasSubDirs = false;

    if (WalkDirectory(dirname, false, false, true) != WALK_DIR_ITEM_RESULT_FAIL)
    {
        strcpy(filename, _filename);
        strcpy(cueFilename, _cueFilename);
        navObjectType = _navObjectType;
        size = _size;
        return true;
    }

    return false;
}

// FindItemIndexByNameAndPathSDNavigator
///////////////////////////////
PROCESS_DIR_ITEM_RESULT FindItemIndexByNameAndPathSDNavigator::ProcessDirectoryItem(const char *filename, const char *path, u_int64_t size, NAV_OBJECT_TYPE navObjectType, char *cueFilename)
{

    //getSearchObject(navTmp, navObjectType, path, _filename);
    // getSearchObject(navTmpTest, navObjectType, path, _filename);

    if (strcmp(_filename, filename) == 0)
    {
        _navObjectType = navObjectType;
        return PROCESS_DIR_ITEM_RESULT_STOP_PROCESSING;    
    }
    
    _counter++;
    return PROCESS_DIR_ITEM_RESULT_PROCEED;
}

int FindItemIndexByNameAndPathSDNavigator::FindItemByNameAndPath(const char *dirname,  const char *filename, NAV_OBJECT_TYPE &navObjectType)
{
    _filename = filename;
    _counter = 0;
    _navObjectType = NAV_OBJECT_NONE;

    if (WalkDirectory(dirname, false, false, true) == WALK_DIR_ITEM_RESULT_STOP_PROCESSING)
    {
        navObjectType = _navObjectType;
        return _counter;
    }

    return -1;
}


// TotalPrefixFilesSDNavigator
///////////////////////////////
PROCESS_DIR_ITEM_RESULT TotalPrefixFilesSDNavigator::ProcessDirectoryItem(const char *filename, const char *path, u_int64_t size, NAV_OBJECT_TYPE navObjectType, char *cueFilename)
{
    if (navObjectType != NAV_OBJECT_DIR && startsWith(filename, _prefix))
    {
        _total++;
    }

    return PROCESS_DIR_ITEM_RESULT_PROCEED;
}

bool TotalPrefixFilesSDNavigator::TotalItems(const char* prefix, int &total)
{
    _prefix = prefix;
    _total = 0;
    _hasSubDirs = false;

    if (WalkDirectory("/", false, false, false) != WALK_DIR_ITEM_RESULT_FAIL)
    {
        total =  _total;
        return true;
    }

    total = -1;
    return false;
}

// PrefixFileByIndexSDNavigator
///////////////////////////////
PROCESS_DIR_ITEM_RESULT PrefixFileByIndexSDNavigator::ProcessDirectoryItem(const char *filename, const char *path, u_int64_t size, NAV_OBJECT_TYPE navObjectType, char *cueFilename)
{
    if (navObjectType != NAV_OBJECT_DIR && startsWith(filename, _prefix))
    {
        if (_counter == _index)
        {
            _size = size;
            _filename = filename;

            return PROCESS_DIR_ITEM_RESULT_STOP_PROCESSING;    
        }

        _counter++;
    }
    return PROCESS_DIR_ITEM_RESULT_PROCEED;
}

bool PrefixFileByIndexSDNavigator::GetFileByIndex(const char *prefix, int index, char* buf, size_t buflen, u_int64_t &size)
{
    _counter = 0;
    _index = index;
    _hasSubDirs = false;
    _prefix = prefix;
    
    if (WalkDirectory("/", false, false, false) != WALK_DIR_ITEM_RESULT_FAIL)
    {
        strcpy(buf, _filename);
        size = _size;
        return true;
    }

    return false;
}

// FileByIndexRecursiveSDNavigator
///////////////////////////////////
PROCESS_DIR_ITEM_RESULT FileByIndexRecursiveSDNavigator::ProcessDirectoryItem(const char *filename, const char *path, u_int64_t size, NAV_OBJECT_TYPE navObjectType, char *cueFilename)
{
    if (_counter == _index)
    {
        _size = size;
        _filename = filename;
        _path = path;

        _cueFilename = cueFilename;
        _navObjectType = navObjectType;
        return PROCESS_DIR_ITEM_RESULT_STOP_PROCESSING;    
    }

    _counter++;
    return PROCESS_DIR_ITEM_RESULT_PROCEED;
}

bool FileByIndexRecursiveSDNavigator::GetObjectByIndexRecursive(const char *dirname, int index, char* filename, char *path, size_t buflen, u_int64_t &size, NAV_OBJECT_TYPE &navObjectType, char *cueFilename)
{
    _counter = 0;
    _index = index;
    _hasSubDirs = false;

    if (WalkDirectory(dirname, true, false, true) != WALK_DIR_ITEM_RESULT_FAIL)
    {
        strcpy(filename, _filename);
        strcpy(path, _path);
        strcpy(cueFilename, _cueFilename);
        navObjectType = _navObjectType;

        size = _size;
        return true;
    }

    return false;
}

// FileByIndexRecursiveSDNavigator
///////////////////////////////////
PROCESS_DIR_ITEM_RESULT FindItemIndexByNameAndPathRecursiveSDNavigator::ProcessDirectoryItem(const char *filename, const char *path, u_int64_t size, NAV_OBJECT_TYPE navObjectType, char *cueFilename)
{
    if ((strcmp(filename, _filename) == 0) && (strcmp(path, _path) == 0))
    {
        return PROCESS_DIR_ITEM_RESULT_STOP_PROCESSING;    
    }

    _counter++;
    return PROCESS_DIR_ITEM_RESULT_PROCEED;
}

int FindItemIndexByNameAndPathRecursiveSDNavigator::FindItemIndexByNameAndPathRecursive(const char *dirname, char* filename, const char *path)
{
    _counter = 0;
    _hasSubDirs = false;
    _filename = filename;
    _path = path;

    if (WalkDirectory(dirname, true, false, true) == WALK_DIR_ITEM_RESULT_STOP_PROCESSING)
    {
        return _counter;
    }

    return -1;
}



// ScanFilesRecursiveSDNavigator
////////////////////////////////
PROCESS_DIR_ITEM_RESULT ScanFilesRecursiveSDNavigator::ProcessDirectoryItem(const char *filename, const char *path, u_int64_t size, NAV_OBJECT_TYPE navObjectType, char *cueFilename)
{
    _callback(_counter, filename, path, size, navObjectType, cueFilename);
    _counter++;

    return PROCESS_DIR_ITEM_RESULT_PROCEED;
}

bool ScanFilesRecursiveSDNavigator::ScanFilesRecursive(const char *dirname, bool &hasDirs, void (*callback)(int, const char *, const char *, u_int64_t, NAV_OBJECT_TYPE, const char *))
{
    _callback = callback;
    _counter = 0;
    _hasSubDirs = false;

    if (WalkDirectory(dirname, true, false, true) != WALK_DIR_ITEM_RESULT_FAIL)
    {
        hasDirs = _hasSubDirs;
        return true;
    }

    return false;
}


