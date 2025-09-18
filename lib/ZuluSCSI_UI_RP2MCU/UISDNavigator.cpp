#include "UISDNavigator.h"
#include "ZuluSCSI_log.h"
#include "ui.h"
#include <ctype.h>

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


// TotalFilesSDNavigator
////////////////////////
PROCESS_DIR_ITEM_RESULT TotalFilesSDNavigator::ProcessDirectoryItem(FsFile &file, const char *filename, const char *path)
{
    _total++;
    return PROCESS_DIR_ITEM_RESULT_PROCEED;
}

bool TotalFilesSDNavigator::TotalItems(const char* dirname, int &total)
{
    _total = 0;
    _hasSubDirs = false;

    if (WalkDirectory(dirname, false, false) != WALK_DIR_ITEM_RESULT_FAIL)
    {
        total =  _total;
        return true;
    }

    total = -1;
    return false;
}

// ItemByIndexSDNavigator
/////////////////////////
PROCESS_DIR_ITEM_RESULT ItemByIndexSDNavigator::ProcessDirectoryItem(FsFile &file, const char *filename, const char *path)
{
    if (_counter == _index)
    {
        _isDir = file.isDir();
        _size = file.size();
        _filename = filename;

        return PROCESS_DIR_ITEM_RESULT_STOP_PROCESSING;    
    }

    _counter++;
    return PROCESS_DIR_ITEM_RESULT_PROCEED;
}

bool ItemByIndexSDNavigator::GetObjectByIndex(const char *dirname, int index, char* filename, size_t buflen, bool &isDir, u_int64_t &size)
{
    _counter = 0;
    _index = index;
    _hasSubDirs = false;

    if (WalkDirectory(dirname, false, false) != WALK_DIR_ITEM_RESULT_FAIL)
    {
        strcpy(filename, _filename);
        isDir = _isDir;
        size = _size;
        return true;
    }

    return false;
}

// FindItemIndexByNameAndPathSDNavigator
///////////////////////////////
PROCESS_DIR_ITEM_RESULT FindItemIndexByNameAndPathSDNavigator::ProcessDirectoryItem(FsFile &file, const char *filename, const char *path)
{
    if (strcmp(filename, _filename) == 0)
    {
        _isDir = file.isDir();
        return PROCESS_DIR_ITEM_RESULT_STOP_PROCESSING;    
    }

    _counter++;
    return PROCESS_DIR_ITEM_RESULT_PROCEED;
}

int FindItemIndexByNameAndPathSDNavigator::FindItemByNameAndPath(const char *dirname,  const char *filename, bool &isDir)
{
    _filename = filename;
    _counter = 0;
    _isDir = false;

    if (WalkDirectory(dirname, false, false) == WALK_DIR_ITEM_RESULT_STOP_PROCESSING)
    {
        isDir = _isDir;
        return _counter;
    }

    return -1;
}


// TotalPrefixFilesSDNavigator
///////////////////////////////
PROCESS_DIR_ITEM_RESULT TotalPrefixFilesSDNavigator::ProcessDirectoryItem(FsFile &file, const char *filename, const char *path)
{
    if (!file.isDir() && startsWith(filename, _prefix))
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

    if (WalkDirectory("/", false, false) != WALK_DIR_ITEM_RESULT_FAIL)
    {
        total =  _total;
        return true;
    }

    total = -1;
    return false;
}

// PrefixFileByIndexSDNavigator
///////////////////////////////
PROCESS_DIR_ITEM_RESULT PrefixFileByIndexSDNavigator::ProcessDirectoryItem(FsFile &file, const char *filename, const char *path)
{
    if (!file.isDir() && startsWith(filename, _prefix))
    {
        if (_counter == _index)
        {
            _size = file.size();
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
    
    if (WalkDirectory("/", false, false) != WALK_DIR_ITEM_RESULT_FAIL)
    {
        strcpy(buf, _filename);
        size = _size;
        return true;
    }

    return false;
}

// FileByIndexRecursiveSDNavigator
///////////////////////////////////
PROCESS_DIR_ITEM_RESULT FileByIndexRecursiveSDNavigator::ProcessDirectoryItem(FsFile &file, const char *filename, const char *path)
{
    if (_counter == _index)
    {
        _size = file.size();
        _filename = filename;
        _path = path;

        return PROCESS_DIR_ITEM_RESULT_STOP_PROCESSING;    
    }

    _counter++;
    return PROCESS_DIR_ITEM_RESULT_PROCEED;
}

bool FileByIndexRecursiveSDNavigator::GetObjectByIndexRecursive(const char *dirname, int index, char* filename, char *path, size_t buflen, u_int64_t &size)
{
    _counter = 0;
    _index = index;
    _hasSubDirs = false;

    if (WalkDirectory(dirname, true, false) != WALK_DIR_ITEM_RESULT_FAIL)
    {
        strcpy(filename, _filename);
        strcpy(path, _path);
        size = _size;
        return true;
    }

    return false;
}

// FileByIndexRecursiveSDNavigator
///////////////////////////////////
PROCESS_DIR_ITEM_RESULT FindItemIndexByNameAndPathRecursiveSDNavigator::ProcessDirectoryItem(FsFile &file, const char *filename, const char *path)
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

    if (WalkDirectory(dirname, true, false) == WALK_DIR_ITEM_RESULT_STOP_PROCESSING)
    {
        return _counter;
    }

    return -1;
}



// ScanFilesRecursiveSDNavigator
////////////////////////////////
PROCESS_DIR_ITEM_RESULT ScanFilesRecursiveSDNavigator::ProcessDirectoryItem(FsFile &file, const char *filename, const char *path)
{
    _callback(_counter, filename, path, file.size());
    _counter++;

    return PROCESS_DIR_ITEM_RESULT_PROCEED;
}

bool ScanFilesRecursiveSDNavigator::ScanFilesRecursive(const char *dirname, bool &hasDirs, void (*callback)(int, const char *, const char *, u_int64_t))
{
    _callback = callback;
    _counter = 0;
    _hasSubDirs = false;

    if (WalkDirectory(dirname, true, false) != WALK_DIR_ITEM_RESULT_FAIL)
    {
        hasDirs = _hasSubDirs;
        return true;
    }

    return false;
}


