#include "SDNavigator.h"
#include "ZuluSCSI_log.h"
#include "ui.h"
#include <ctype.h>

char g_tmpFilename[MAX_PATH_LEN];
char g_tmpFilepath[MAX_PATH_LEN];

FindItemIndexByNameAndPathSDNavigatorRaw SDNavFindItemIndexByNameAndPathRaw;
ScanFilesSDNavigatorRaw SDNavScanFilesRaw;
GetFirstFileRecursiveSDNavigator SDNavGetFirstFileRecursive;

// Bin Cue

int totCue = 0;
char tmpCueFile[MAX_FILE_PATH];
u_int64_t g_cueSize;

void cueScan(int count, const char *file, const char *path, u_int64_t size)
{
  const char *extension = strrchr(file, '.');
  if (extension)
  {
    if (strcasecmp(extension, ".cue") == 0)
    {
        strcpy(tmpCueFile, path);
        strcat(tmpCueFile, "/");
        strcat(tmpCueFile, file);

        strcpy(g_tmpFilepath, tmpCueFile);

        strcpy(g_tmpFilename, file);
        g_cueSize = size;
        totCue++;
    }
  }
}


// Load data from CUE sheet for the given device,
// using the second half of scsiDev.data buffer for temporary storage.
// Returns false if no cue sheet or it could not be opened.
static bool loadCueSheet(const char *cueFile, CUEParser &parser, u_int64_t &cueFileSize)
{
    // Use second half of scsiDev.data as the buffer for cue sheet text
    size_t halfbufsize = sizeof(scsiDev.data) / 2;
    char *cuebuf = (char*)&scsiDev.data[halfbufsize];

    FsVolume *vol = SD.vol();
    FsFile fHandle = vol->open(cueFile, O_RDONLY);
    cueFileSize = fHandle.read(cuebuf, halfbufsize);
    fHandle.close();

    if (cueFileSize <= 0 || cueFileSize < g_cueSize) // empty, or didn't load the whole thing
    {
        return false;
    }

    cuebuf[cueFileSize] = '\0';
    parser = CUEParser(cuebuf);
    return true;
}

bool ProcessCueFile(char *cueFile, u_int64_t &cueSize, u_int64_t &binSize, int &totalBins, bool matchBins, const char *folder)
{
    CUEParser parser;
    u_int64_t cueFileSize = 0;
    totalBins = 0;

    if (loadCueSheet(cueFile, parser, cueFileSize))
    {
        CUETrackInfo const *track;
        uint64_t prev_capacity = 0;

        // Find last track
        while((track = parser.next_track(prev_capacity)) != nullptr)
        {
            if (matchBins)
            {
                NAV_OBJECT_TYPE navObjectType;
                u_int64_t size;
                int index = SDNavFindItemIndexByNameAndPathRaw.FindItemByNameAndPath(folder, track->filename, size, navObjectType);
                
                if (index == -1)
                {
                    return false; // Couldn't find a bin mentioned in the cue file
                }
                binSize += size;
            }
            
            totalBins++;
        }
    }
    else
    {
        return false;
    }

    cueSize = cueFileSize;

    return true;
}

bool isFileABinFileWithACueFile(const char *binFile, char *cueFile, u_int64_t &cueSize, int &totalBins)
{
    const char *extension = strrchr(binFile, '.');
    if (!extension || strcasecmp(extension, ".bin") != 0)
    {
        return false;
    }

    char tmp[MAX_FILE_PATH];
    strcpy(tmp, binFile);
    strcpy(tmp+(extension-binFile), ".cue");

    if (SD.exists(tmp))
    {
        u_int64_t binSize;
        bool res = ProcessCueFile(tmp, cueSize, binSize, totalBins, false, nullptr);
        if (res)
        {
            const char *slash = strrchr(tmp, '/');
            if (slash)
            {
                strcpy(cueFile, slash+1);
            }
        }
        return res;
    }
    return false;
}

bool isFolderACueBinSet(const char *folder, char *cueFile, u_int64_t &cueSize, u_int64_t &binSize, int &totalBins)
{
  // look for .cue files
  totCue = 0;
  binSize = 0;
  SDNavScanFilesRaw.ScanFiles(folder, cueScan);

  if (totCue == 1)
  {
    bool res = ProcessCueFile(g_tmpFilepath, cueSize, binSize, totalBins, true, folder);
    if (res)
    {
        strcpy(cueFile, g_tmpFilename);
    }
    return res;
  }
    
  return false; // Didn't find exactly 1 cue file
}


bool SDNavigator::startsWith(const char* str, const char* prefix) 
{
    size_t prefix_len = strlen(prefix);
    if (strlen(str) < prefix_len)
        return false;
    return strncasecmp(prefix, str, prefix_len) == 0;
}

PROCESS_DIR_ITEM_RESULT SDNavigator::ProcessDirectoryItem(const char *filename, const char *path, u_int64_t size, NAV_OBJECT_TYPE navObjectType, char *cueFilename)
{
    return PROCESS_DIR_ITEM_RESULT_STOP_PROCESSING;
}

WALK_DIR_RESULT SDNavigator::WalkDirectory(const char* dirname, bool recursive, bool includeAllFiles, bool remapBinCues)
{
    char buf[MAX_PATH_LEN];
   

    FsFile dir;
    if (dirname[0] == '\0')
    {
       // SAVE logmsg("Image directory name invalid");
        return WALK_DIR_ITEM_RESULT_FAIL;
    }
    if (!dir.open(dirname))
    {
      // SAVE  logmsg("Image directory '", dirname, "' couldn't be opened");
        return WALK_DIR_ITEM_RESULT_FAIL;
    }
    if (!dir.isDir())
    {
     // SAVE   logmsg("Can't find images in '", dirname, "', not a directory");
        dir.close();
        return WALK_DIR_ITEM_RESULT_FAIL;
    }
    if (dir.isHidden())
    {
      // SAVE  logmsg("Image directory '", dirname, "' is hidden, skipping");
        dir.close();
        return WALK_DIR_ITEM_RESULT_FAIL;
    }

    FsFile file;
    while (file.openNext(&dir, O_RDONLY))
    {
        memset(buf, 0, MAX_FILE_PATH);
        if (!file.getName(buf, MAX_FILE_PATH))
        {
     // SAVE       logmsg("Image directory '", dirname, "' had invalid file");
            continue;
        }

        if (!scsiDiskFilenameValid(buf) && !includeAllFiles) 
        {
            continue;
        }
        if (file.isHidden()) 
        {
            // This is commented out as 'Image '//System Volume Information' is hidden, skipping file'
            // was spamming the log
            // logmsg("Image '", dirname, "/", buf, "' is hidden, skipping file");
            continue;
        }

        char newPath[MAX_FILE_PATH];
        char cueFile[MAX_FILE_PATH];
        memset(cueFile, 0, MAX_FILE_PATH);

        u_int64_t cueSize;
        u_int64_t binSize;
        int totalBins;

        if (recursive)
        {
            memset(newPath, 0, MAX_FILE_PATH);
            strcpy(newPath, dirname);
            strcat(newPath, "/");
            strcat(newPath, buf);
            
            if (!file.isDir()) // File
            {
                bool isSimpleCue = isFileABinFileWithACueFile(newPath, cueFile, cueSize, totalBins);

                switch(ProcessDirectoryItem(buf, dirname, file.size(), isSimpleCue ? NAV_OBJECT_CUE_SIMPLE : NAV_OBJECT_FILE, cueFile))
                {
                    case PROCESS_DIR_ITEM_RESULT_PROCEED:
                        break;
                    case PROCESS_DIR_ITEM_RESULT_STOP_PROCESSING:
                        return WALK_DIR_ITEM_RESULT_STOP_PROCESSING;
                }
            }
            else // Dir
            {
                bool processDirAsNormal = true;

                if (remapBinCues)
                {
                    // do check
                    if (isFolderACueBinSet(newPath, cueFile, cueSize, binSize, totalBins))
                    {
                        processDirAsNormal = false;

                        switch(ProcessDirectoryItem(buf, dirname, binSize, NAV_OBJECT_CUE, cueFile))
                        {
                            case PROCESS_DIR_ITEM_RESULT_PROCEED:
                                break;
                            case PROCESS_DIR_ITEM_RESULT_STOP_PROCESSING:
                                return WALK_DIR_ITEM_RESULT_STOP_PROCESSING;
                        }
                    }
                }
                
                if (processDirAsNormal)
                {
                    _hasSubDirs = true;

                    switch(WalkDirectory(newPath, recursive, includeAllFiles, remapBinCues))
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
        }
        else
        {
            // Non Recursive
            bool processDirAsNormal = true;

            if (remapBinCues && file.isDir())
            {
                memset(newPath, 0, MAX_FILE_PATH);
                strcpy(newPath, dirname);
                strcat(newPath, "/");
                strcat(newPath, buf);
                
                if (isFolderACueBinSet(newPath, cueFile, cueSize, binSize, totalBins))
                {
                    processDirAsNormal = false;

                    switch(ProcessDirectoryItem(buf, dirname, binSize, NAV_OBJECT_CUE, cueFile))
                    {
                        case PROCESS_DIR_ITEM_RESULT_PROCEED:
                            break;
                        case PROCESS_DIR_ITEM_RESULT_STOP_PROCESSING:
                            return WALK_DIR_ITEM_RESULT_STOP_PROCESSING;
                    }    
                }
            }

            if (processDirAsNormal)
            {
                NAV_OBJECT_TYPE type = NAV_OBJECT_DIR;
                
                if (!file.isDir())
                {
                    bool isSimpleCue = isFileABinFileWithACueFile(newPath, cueFile, cueSize, totalBins);
                    type = isSimpleCue ? NAV_OBJECT_CUE_SIMPLE : NAV_OBJECT_FILE;
                }

                switch(ProcessDirectoryItem(buf, dirname, file.size(), type, cueFile))
                {
                    case PROCESS_DIR_ITEM_RESULT_PROCEED:
                        break;
                    case PROCESS_DIR_ITEM_RESULT_STOP_PROCESSING:
                        return WALK_DIR_ITEM_RESULT_STOP_PROCESSING;
                }
            }
        }
    }

    return WALK_DIR_ITEM_RESULT_OK;
}

// GetFirstFileRecursiveSDNavigator
///////////////////////////////////
PROCESS_DIR_ITEM_RESULT GetFirstFileRecursiveSDNavigator::ProcessDirectoryItem(const char *filename, const char *path, u_int64_t size, NAV_OBJECT_TYPE navObjectType, char *cueFilename)
{
    _filename = filename;
    _path = path;
    
    return PROCESS_DIR_ITEM_RESULT_STOP_PROCESSING;
}

bool GetFirstFileRecursiveSDNavigator::GetFirstFileRecursive(const char *dirname, char *filename, char *path)
{
    if (WalkDirectory(dirname, true, false, true) != WALK_DIR_ITEM_RESULT_FAIL)
    {
        strcpy(filename, _filename);
        strcpy(path, _path);
        return true;
    }

    return false;
}

// FindItemIndexByNameAndPathSDNavigator
///////////////////////////////
PROCESS_DIR_ITEM_RESULT FindItemIndexByNameAndPathSDNavigatorRaw::ProcessDirectoryItem(const char *filename, const char *path, u_int64_t size, NAV_OBJECT_TYPE navObjectType, char *cueFilename)
{
    if (strcasecmp(filename, _filename) == 0)
    {
        _navObjectType = navObjectType;
        _size = size;
        return PROCESS_DIR_ITEM_RESULT_STOP_PROCESSING;    
    }

    _counter++;
    return PROCESS_DIR_ITEM_RESULT_PROCEED;
}

int FindItemIndexByNameAndPathSDNavigatorRaw::FindItemByNameAndPath(const char *dirname,  const char *filename,u_int64_t &size, NAV_OBJECT_TYPE &navObjectType)
{
    _filename = filename;
    _counter = 0;
    _navObjectType = NAV_OBJECT_NONE;

    if (WalkDirectory(dirname, false, false, false) == WALK_DIR_ITEM_RESULT_STOP_PROCESSING)
    {
        navObjectType = _navObjectType;
        size = _size;
        return _counter;
    }

    return -1;
}

// ScanFilesRecursiveSDNavigator
////////////////////////////////
PROCESS_DIR_ITEM_RESULT ScanFilesSDNavigatorRaw::ProcessDirectoryItem(const char *filename, const char *path, u_int64_t size, NAV_OBJECT_TYPE navObjectType, char *cueFilename)
{
    _callback(_counter, filename, path, size);
    _counter++;

    return PROCESS_DIR_ITEM_RESULT_PROCEED;
}

bool ScanFilesSDNavigatorRaw::ScanFiles(const char *dirname, void (*callback)(int, const char *, const char *, u_int64_t))
{
    _callback = callback;
    _counter = 0;

    if (WalkDirectory(dirname, false, true, false) != WALK_DIR_ITEM_RESULT_FAIL)
    {
        return true;
    }

    return false;
}
