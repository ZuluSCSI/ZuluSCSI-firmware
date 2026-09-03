// Custom .ini file access caching layer for minIni.
// This reduces boot delay by only reading the ini file once
// after boot or SD-card removal.

#include <minGlue.h>
#include <minIni_cache.h>
#include <SdFat.h>
#include <string.h>

// This can be overridden in platformio.ini
// Set to 0 to disable the cache.
#ifndef INI_CACHE_SIZE
#define INI_CACHE_SIZE 4096
#endif

// Use the SdFs instance from main program
extern SdFs SD;

// Called by the main program while the .ini file is read from the SD card
static ini_keep_alive_callback_t g_ini_keep_alive_callback = nullptr;

static struct {
    bool valid;
    INI_FILETYPE *fp;

#if INI_CACHE_SIZE > 0
    const char *filename;
    uint32_t filelen;
    uint32_t cachelen;
    INI_FILEPOS current_pos;
    char cachedata[INI_CACHE_SIZE];
#endif
} g_ini_cache;

// Invalidate any cached file contents
void invalidate_ini_cache()
{
    g_ini_cache.valid = false;
    g_ini_cache.fp = nullptr;

#if INI_CACHE_SIZE > 0
    g_ini_cache.filelen = 0;
    g_ini_cache.cachelen = 0;
#endif
}

#if INI_CACHE_SIZE > 0

// Line buffer used while filling the cache.
// Lines longer than this are stored unmodified, because minIni cannot parse
// them in one piece either (it reads at most INI_BUFFERSIZE bytes per line).
#ifndef INI_CACHE_LINE_SIZE
#define INI_CACHE_LINE_SIZE 512
#endif

// Whitespace as minIni's skipleading() / skiptrailing() see it.
static bool is_ini_space(char c)
{
    return c > '\0' && c <= ' ';
}

// Find where a trailing comment starts inside a value.
// Uses the same quoting and escaping rules as minIni's cleanstring(), so a
// ';' or '#' inside a "quoted string", doubled as "" or escaped as \" is kept.
// Returns the index of the comment character, or 'end' if there is no comment.
static size_t find_value_comment(const char *line, size_t pos, size_t end)
{
    bool isstring = false;

    while (pos < end)
    {
        char c = line[pos];

        if ((c == ';' || c == '#') && !isstring)
        {
            break;
        }

        if (c == '"')
        {
            if (pos + 1 < end && line[pos + 1] == '"')
                pos++;                  // skip "" (both quotes)
            else
                isstring = !isstring;   // single quote, toggle isstring
        }
        else if (c == '\\' && pos + 1 < end && line[pos + 1] == '"')
        {
            pos++;                      // skip \" (both quotes)
        }

        pos++;
    }

    return pos;
}

// Copy a single line into the cache with comments and surrounding whitespace
// removed. Blank lines and comment-only lines are dropped entirely.
// Returns the number of bytes written to dst, or -1 if dst is too small.
static int strip_ini_line(const char *line, size_t linelen, char *dst, size_t dstsize)
{
    // Drop leading whitespace and the trailing line terminator
    size_t start = 0;
    size_t end = linelen;
    while (start < end && is_ini_space(line[start]))
        start++;
    while (end > start && is_ini_space(line[end - 1]))
        end--;

    // Empty line, or a line that is nothing but a comment
    if (start == end || line[start] == ';' || line[start] == '#')
    {
        return 0;
    }

    size_t content_end = end;

    if (line[start] == '[')
    {
        // Section header. minIni takes the section name from the last ']' on
        // the line, so anything following it is a trailing comment.
        // A line without ']' is not a valid header and is kept unmodified.
        size_t bracket = end;
        while (bracket > start && line[bracket - 1] != ']')
            bracket--;

        if (bracket > start)
        {
            content_end = bracket;
        }
    }
    else
    {
        // Possible "key = value" or "key : value" entry. minIni looks for a
        // comment only in the value, so the key is copied as-is.
        // A line without a separator is not an entry and is kept unmodified.
        const char *sep = (const char *)memchr(line + start, '=', end - start);
        if (sep == nullptr)
            sep = (const char *)memchr(line + start, ':', end - start);

        if (sep != nullptr)
        {
            content_end = find_value_comment(line, (size_t)(sep - line) + 1, end);
        }
    }

    // Drop the whitespace that separated the content from a removed comment
    while (content_end > start && is_ini_space(line[content_end - 1]))
        content_end--;

    size_t len = content_end - start;
    if (len == 0)
    {
        return 0;
    }

    if (len + 1 > dstsize)
    {
        return -1;
    }

    memcpy(dst, line + start, len);
    dst[len] = '\n';
    return (int)(len + 1);
}

#endif

// Read the config file into RAM
void reload_ini_cache(const char *filename)
{
    g_ini_cache.valid = false;
    g_ini_cache.fp = nullptr;

#if INI_CACHE_SIZE > 0
    g_ini_cache.filename = filename;
    g_ini_cache.filelen = 0;
    g_ini_cache.cachelen = 0;

    FsFile config = SD.open(filename, O_RDONLY);
    if (!config.isOpen())
    {
        // Not an error: without a config file the firmware uses its defaults.
        return;
    }

    g_ini_cache.filelen = config.fileSize();

    // Read the file one line at a time and store only what minIni parses.
    // Leaving out comments and blank lines lets .ini files that are larger
    // than the cache still be served from RAM.
    char linebuf[INI_CACHE_LINE_SIZE];
    uint32_t cachepos = 0;
    bool copy_verbatim = false;
    bool ok = true;

    while (ok)
    {
        int linelen = config.fgets(linebuf, sizeof(linebuf));

        if (linelen == 0)
        {
            break; // End of file
        }
        else if (linelen < 0)
        {
            ok = false; // Read error, fall back to reading from SD card
            break;
        }

        // A full buffer without a line terminator means the line continues in
        // the next read. Such lines are stored unmodified, as a partial line
        // cannot be checked for quotes and comments reliably.
        bool truncated = (linelen == (int)sizeof(linebuf) - 1 && linebuf[linelen - 1] != '\n');

        if (copy_verbatim || truncated)
        {
            if (cachepos + (uint32_t)linelen > INI_CACHE_SIZE)
            {
                ok = false;
            }
            else
            {
                memcpy(&g_ini_cache.cachedata[cachepos], linebuf, linelen);
                cachepos += linelen;
                copy_verbatim = truncated;
            }
        }
        else
        {
            int written = strip_ini_line(linebuf, linelen,
                                         &g_ini_cache.cachedata[cachepos],
                                         INI_CACHE_SIZE - cachepos);
            if (written < 0)
            {
                ok = false; // Does not fit in the cache
            }
            else
            {
                cachepos += written;
            }
        }
    }

    config.close();

    if (ok)
    {
        g_ini_cache.cachelen = cachepos;
        g_ini_cache.valid = true;
    }
#endif
}

// Get the length of the .ini file as stored on the SD card
uint32_t get_ini_file_length()
{
#if INI_CACHE_SIZE > 0
    return g_ini_cache.filelen;
#else
    return 0;
#endif
}

// Get the length of the cached copy of the .ini file
uint32_t get_ini_cache_length()
{
#if INI_CACHE_SIZE > 0
    if (g_ini_cache.valid)
    {
        return g_ini_cache.cachelen;
    }
#endif

    return 0;
}

// Get the total space available for caching the .ini file
uint32_t get_ini_cache_capacity()
{
    return INI_CACHE_SIZE;
}

// Check whether the .ini file is being served from the cache
bool is_ini_cached()
{
    return g_ini_cache.valid;
}

// Write the cached copy of the .ini file to the SD card
bool dump_ini_cache(const char *filename)
{
#if INI_CACHE_SIZE > 0
    if (!g_ini_cache.valid)
    {
        return false;
    }

    FsFile dump = SD.open(filename, O_WRONLY | O_CREAT | O_TRUNC);
    if (!dump.isOpen())
    {
        return false;
    }

    bool ok = (dump.write(g_ini_cache.cachedata, g_ini_cache.cachelen) == g_ini_cache.cachelen);
    ok = dump.sync() && ok;
    dump.close();
    return ok;
#else
    (void)filename;
    return false;
#endif
}

// Set the callback used while reading an .ini file that is not cached
void set_ini_keep_alive_callback(ini_keep_alive_callback_t callback)
{
    g_ini_keep_alive_callback = callback;
}

// Open .ini file either from cache or from SD card
bool ini_openread(const char *filename, INI_FILETYPE *fp)
{
#if INI_CACHE_SIZE > 0
    if (g_ini_cache.valid &&
        (filename == g_ini_cache.filename || strcmp(filename, g_ini_cache.filename) == 0))
    {
        fp->close();
        g_ini_cache.fp = fp;
        g_ini_cache.current_pos.position = 0;
        return true;
    }
#endif

    // The file did not fit in the cache, so it is opened and scanned from the
    // SD card once per setting that is read. On a large .ini file that adds up
    // to more time than the watchdog allows, even though the firmware is still
    // making progress, so let the main program keep the watchdog fed here.
    if (g_ini_keep_alive_callback != nullptr)
    {
        g_ini_keep_alive_callback();
    }

    return fp->open(SD.vol(), filename, O_RDONLY);
}

// Close previously opened file
bool ini_close(INI_FILETYPE *fp)
{
#if INI_CACHE_SIZE > 0
    if (g_ini_cache.fp == fp)
    {
        g_ini_cache.fp = nullptr;
        return true;
    }
    else
#endif
    {
        return fp->close();
    }
}

// Read a single line from cache or from SD card
bool ini_read(char *buffer, int size, INI_FILETYPE *fp)
{
#if INI_CACHE_SIZE > 0
    if (g_ini_cache.fp == fp)
    {
        // Read one line from cache
        uint32_t srcpos = g_ini_cache.current_pos.position;
        int dstpos = 0;
        while (srcpos < g_ini_cache.cachelen &&
               dstpos < size - 1)
        {
            char b = g_ini_cache.cachedata[srcpos++];
            buffer[dstpos++] = b;

            if (b == '\n') break;
        }
        buffer[dstpos] = 0;
        g_ini_cache.current_pos.position = srcpos;
        return dstpos > 0;
    }
    else
#endif
    {
        // Read from SD card
        return fp->fgets(buffer, size) > 0;
    }
}

// Get the position inside the file
void ini_tell(INI_FILETYPE *fp, INI_FILEPOS *pos)
{
#if INI_CACHE_SIZE > 0
    if (g_ini_cache.fp == fp)
    {
        *pos = g_ini_cache.current_pos;
    }
    else
#endif
    {
        fp->fgetpos(pos);
    }
}

// Go back to previously saved position
void ini_seek(INI_FILETYPE *fp, INI_FILEPOS *pos)
{
#if INI_CACHE_SIZE > 0
    if (g_ini_cache.fp == fp)
    {
        g_ini_cache.current_pos = *pos;
    }
    else
#endif
    {
        fp->fsetpos(pos);
    }
}
