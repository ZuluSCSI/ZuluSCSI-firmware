#include "SDNavigator.h"
#include "ZuluSCSI_log.h"
#include "ui.h"
#include <ctype.h>

bool SDNavigator::startsWith(const char* str, const char* prefix) 
{
    // Loop until the prefix character is null or a mismatch is found
    while (tolower(*prefix) && tolower(*str) == tolower(*prefix)) 
    {
        str++;
        prefix++;
    }
    // If the prefix is fully matched (meaning it reached its null terminator)
    return *prefix == '\0'; // or return *prefix == 0;
}

PROCESS_DIR_ITEM_RESULT SDNavigator::ProcessDirectoryItem(FsFile &file, const char *filename, const char *path)
{
    return PROCESS_DIR_ITEM_RESULT_STOP_PROCESSING;
}

WALK_DIR_RESULT SDNavigator::WalkDirectory(const char* dirname, bool recursive)
{
    char buf[MAX_PATH_LEN];

    FsFile dir;
    if (dirname[0] == '\0')
    {
        logmsg("Image directory name invalid");
        return WALK_DIR_ITEM_RESULT_FAIL;
    }
    if (!dir.open(dirname))
    {
        logmsg("Image directory '", dirname, "' couldn't be opened");
        return WALK_DIR_ITEM_RESULT_FAIL;
    }
    if (!dir.isDir())
    {
        logmsg("Can't find images in '", dirname, "', not a directory");
        dir.close();
        return WALK_DIR_ITEM_RESULT_FAIL;
    }
    if (dir.isHidden())
    {
        logmsg("Image directory '", dirname, "' is hidden, skipping");
        dir.close();
        return WALK_DIR_ITEM_RESULT_FAIL;
    }

    FsFile file;
    while (file.openNext(&dir, O_RDONLY))
    {
        if (!file.getName(buf, MAX_FILE_PATH))
        {
            logmsg("Image directory '", dirname, "' had invalid file");
            continue;
        }

        if (!scsiDiskFilenameValid(buf)) 
        {
            continue;
        }
        if (file.isHidden()) 
        {
            logmsg("Image '", dirname, "/", buf, "' is hidden, skipping file");
            continue;
        }

        if (recursive)
        {
            if (!file.isDir())
            {
                switch(ProcessDirectoryItem(file, buf, dirname))
                {
                    case PROCESS_DIR_ITEM_RESULT_PROCEED:
                        break;
                    case PROCESS_DIR_ITEM_RESULT_STOP_PROCESSING:
                        return WALK_DIR_ITEM_RESULT_STOP_PROCESSING;
                }
            }
            else
            {
                char newPath[MAX_FILE_PATH];
                strcpy(newPath, dirname);
                strcat(newPath, "/");
                strcat(newPath, buf);

                _hasSubDirs = true;

                switch(WalkDirectory(newPath, recursive))
                {
                    case WALK_DIR_ITEM_RESULT_OK:
                        break;
                    case WALK_DIR_ITEM_RESULT_FAIL:
                        return WALK_DIR_ITEM_RESULT_FAIL;
                    case WALK_DIR_ITEM_RESULT_STOP_PROCESSING:
                        return WALK_DIR_ITEM_RESULT_STOP_PROCESSING;
                }
            }
        }
        else
        {
            switch(ProcessDirectoryItem(file, buf, dirname))
            {
                case PROCESS_DIR_ITEM_RESULT_PROCEED:
                    break;
                case PROCESS_DIR_ITEM_RESULT_STOP_PROCESSING:
                    return WALK_DIR_ITEM_RESULT_STOP_PROCESSING;
            }
        }
    }

    return WALK_DIR_ITEM_RESULT_OK;
}


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

    if (WalkDirectory(dirname, false) != WALK_DIR_ITEM_RESULT_FAIL)
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

bool ItemByIndexSDNavigator::GetObjectByIndex(const char *dirname, int index, char* buf, size_t buflen, bool &isDir, u_int64_t &size)
{
    _counter = 0;
    _index = index;
    _hasSubDirs = false;

    if (WalkDirectory(dirname, false) != WALK_DIR_ITEM_RESULT_FAIL)
    {
        strcpy(buf, _filename);
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

    if (WalkDirectory(dirname, false) == WALK_DIR_ITEM_RESULT_STOP_PROCESSING)
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

    if (WalkDirectory("/", false) != WALK_DIR_ITEM_RESULT_FAIL)
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

    if (WalkDirectory("/", false) != WALK_DIR_ITEM_RESULT_FAIL)
    {
        strcpy(buf, _filename);
        size = _size;
        return true;
    }

    return false;
}


// TotalFilesRecursiveSDNavigator
/////////////////////////////////
PROCESS_DIR_ITEM_RESULT TotalFilesRecursiveSDNavigator::ProcessDirectoryItem(FsFile &file, const char *filename, const char *path)
{
    _total++;
    return PROCESS_DIR_ITEM_RESULT_PROCEED;
}

bool TotalFilesRecursiveSDNavigator::TotalItemsRecursive(const char* dirname, int &total)
{
    _total = 0;
    _hasSubDirs = false;

    if (WalkDirectory(dirname, true) != WALK_DIR_ITEM_RESULT_FAIL)
    {
        total =  _total;
        return true;
    }

    total = -1;
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

    if (WalkDirectory(dirname, true) != WALK_DIR_ITEM_RESULT_FAIL)
    {
        strcpy(filename, _filename);
        strcpy(path, _path);
        size = _size;
        return true;
    }

    return false;
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

    if (WalkDirectory(dirname, true) != WALK_DIR_ITEM_RESULT_FAIL)
    {
        hasDirs = _hasSubDirs;
        return true;
    }

    return false;
}

// GetFirstFileRecursiveSDNavigator
///////////////////////////////////
PROCESS_DIR_ITEM_RESULT GetFirstFileRecursiveSDNavigator::ProcessDirectoryItem(FsFile &file, const char *filename, const char *path)
{
    _filename = filename;
    _path = path;

    return PROCESS_DIR_ITEM_RESULT_STOP_PROCESSING;
}

bool GetFirstFileRecursiveSDNavigator::GetFirstFileRecursive(const char *dirname, char *filename, char *path)
{
    if (WalkDirectory(dirname, true) != WALK_DIR_ITEM_RESULT_FAIL)
    {
        strcpy(filename, _path);
        strcat(filename, "/");
        strcat(filename, _filename);
        
        strcpy(path, _path);

        return true;
    }

    return false;
}