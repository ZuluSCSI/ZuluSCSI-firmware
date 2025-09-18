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
        totCue++;
    }
  }
}


// Load data from CUE sheet for the given device,
// using the second half of scsiDev.data buffer for temporary storage.
// Returns false if no cue sheet or it could not be opened.
static bool loadCueSheet(const char *cueFile, CUEParser &parser)
{
    // Use second half of scsiDev.data as the buffer for cue sheet text
    size_t halfbufsize = sizeof(scsiDev.data) / 2;
    char *cuebuf = (char*)&scsiDev.data[halfbufsize];

    FsVolume *vol = SD.vol();
    FsFile fHandle = vol->open(cueFile, O_RDONLY);
    int len = fHandle.read(cuebuf, halfbufsize);
    fHandle.close();

    if (len <= 0)
    {
        return false;
    }

    cuebuf[len] = '\0';
    parser = CUEParser(cuebuf);
    return true;
}

bool isFolderACueBinSet(const char *folder, char *cueFile)
{
  // look for .cue files
  totCue = 0;
  SDNavScanFilesRaw.ScanFiles(folder, cueScan);

  if (totCue == 1)
  {
    // found 1 cue file
    // parse it, and look for the bin files in it
    CUEParser parser;
    if (loadCueSheet(g_tmpFilepath, parser))
    {
      CUETrackInfo const *track;
      uint64_t prev_capacity = 0;

      // Find last track
      while((track = parser.next_track(prev_capacity)) != nullptr)
      {
          bool isDir;
          int index = SDNavFindItemIndexByNameAndPathRaw.FindItemByNameAndPath(folder, track->filename, isDir);
          if (index == -1)
          {
            return false; // Couldn't find a bin mentioned in the cue file
          }
      }
    }

    strcpy(cueFile, g_tmpFilename);

    return true; // All matched
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

PROCESS_DIR_ITEM_RESULT SDNavigator::ProcessDirectoryItem(FsFile &file, const char *filename, const char *path)
{
    return PROCESS_DIR_ITEM_RESULT_STOP_PROCESSING;
}

WALK_DIR_RESULT SDNavigator::WalkDirectory(const char* dirname, bool recursive, bool includeAllFiles)
{
    char buf[MAX_PATH_LEN];
    memset(buf, 0, MAX_FILE_PATH);

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
                memset(newPath, 0, MAX_FILE_PATH);
                strcpy(newPath, dirname);
                strcat(newPath, "/");
                strcat(newPath, buf);

                _hasSubDirs = true;

                switch(WalkDirectory(newPath, recursive, includeAllFiles))
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
    if (WalkDirectory(dirname, true, false) != WALK_DIR_ITEM_RESULT_FAIL)
    {
        strcpy(filename, _path);
        strcat(filename, "/");
        strcat(filename, _filename);
        
        strcpy(path, _path);

        return true;
    }

    return false;
}

// FindItemIndexByNameAndPathSDNavigator
///////////////////////////////
PROCESS_DIR_ITEM_RESULT FindItemIndexByNameAndPathSDNavigatorRaw::ProcessDirectoryItem(FsFile &file, const char *filename, const char *path)
{
    if (strcmp(filename, _filename) == 0)
    {
        _isDir = file.isDir();
        return PROCESS_DIR_ITEM_RESULT_STOP_PROCESSING;    
    }

    _counter++;
    return PROCESS_DIR_ITEM_RESULT_PROCEED;
}

int FindItemIndexByNameAndPathSDNavigatorRaw::FindItemByNameAndPath(const char *dirname,  const char *filename, bool &isDir)
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

// ScanFilesRecursiveSDNavigator
////////////////////////////////
PROCESS_DIR_ITEM_RESULT ScanFilesSDNavigatorRaw::ProcessDirectoryItem(FsFile &file, const char *filename, const char *path)
{
    _callback(_counter, filename, path, file.size());
    _counter++;

    return PROCESS_DIR_ITEM_RESULT_PROCEED;
}

bool ScanFilesSDNavigatorRaw::ScanFiles(const char *dirname, void (*callback)(int, const char *, const char *, u_int64_t))
{
    _callback = callback;
    _counter = 0;

    if (WalkDirectory(dirname, false, true) != WALK_DIR_ITEM_RESULT_FAIL)
    {
        return true;
    }

    return false;
}
