#include "COWStorage.h"

#if ENABLE_COW

#include <cassert>
#include "ZuluSCSI_log.h"
#include "ZuluSCSI.h"
#include <SdFat.h>

#include "ImageBackingStore.h"  //  Only for global SD object

uint8_t *g_cow_buffer;      // Pre-allocated buffer for copy operations
uint32_t g_cow_buffer_size; // Size of COW buffer

//	Helper function: allocate the global COW buffer for cow->dirty copy during SCSI writes
static void allocateCOWBuffer()
{
    if (g_cow_buffer == nullptr)
    {
        g_cow_buffer_size = g_scsi_settings.getSystem()->cowBufferSize;
        g_cow_buffer = (uint8_t*)malloc( g_cow_buffer_size );
        assert(g_cow_buffer != nullptr);
    }
}

//	Helper function: create a new image file of specified size if needed
static bool createDirtyFile(const char *dirty_filename, uint64_t size)
{
    //  If file already exists and is same size or larger, do nothing
    FsFile file;
    file.open(dirty_filename, O_RDONLY);
    if (file.isOpen()) // file exists
    {
        uint64_t existing_size = file.size();
        file.close();
        if (existing_size >= size) // size ok
        {
            return true;
        }
        logmsg("---- Dirty file exists but is too small, recreating: ", dirty_filename);
        SD.remove(dirty_filename);
    }
    logmsg("---- COW creating ", (int) (size / 1048576), " MB dirty file: ", dirty_filename);
    return createImageFile(dirty_filename, size);
}

COWStorage::~COWStorage()
{
	cleanup();
}

// Initialize COW storage for the given image file
bool COWStorage::initialize(const char *filename, uint32_t scsi_block_size, scsi_device_settings_t *device_settings)
{
    size_t len = strlen(filename);
    if (len <= 3 || strcasecmp(filename + len - 4, ".cow") != 0)
	{
		// Not a .cow file
		return false;
	}

	// Generate dirty filename by replacing .cow extension with .tmp
	char dirty_filename[MAX_FILE_PATH + 1];
	strncpy(dirty_filename, filename, len - 4);
	dirty_filename[len - 4] = '\0';
	strcat(dirty_filename, ".tmp");

	auto bitmap_max_size = device_settings->cowBitmapSize;

    logmsg("---- Initializing COW for image: ", filename);
    logmsg("---- Bitmap max size: ", (int)bitmap_max_size, " bytes");

    allocateCOWBuffer();

    m_scsi_block_size_cow = scsi_block_size;
    m_current_position_cow = 0;

    // Open files
    m_fsfile.open(filename, O_RDONLY);
    if (!createDirtyFile(dirty_filename, m_fsfile.size()))
    {
        logmsg("---- COW initialization failed: could not create dirty file");
        return false;
    }
    m_fsfile_dirty.open(dirty_filename, O_RDWR);

    // Calculate image size in sectors
    uint64_t image_size_bytes = m_fsfile.size();
    uint32_t total_sectors = image_size_bytes / scsi_block_size;

    // Create overlay file at the same size as original (sparse)
    m_fsfile_dirty.seek(image_size_bytes - 1);
    uint8_t zero = 0;
    ssize_t written = m_fsfile_dirty.write(&zero, 1); // Create sparse file of correct size
    if (written != 1)
    {
        logmsg("---- COW initialization failed: dirty file write error");
        return false;
    }

    //  We may need to repeat the calculations if the bitmap size if too large to be allocated
    do
    {
        // Calculate optimal group size based on provided bitmap size
        // We have bitmap_size * 8 bits available in our bitmap
        uint32_t max_groups = bitmap_max_size * 8;

        // Calculate group size - must be multiple of 512 sectors and fit within bitmap
        m_cow_group_size = ((total_sectors + max_groups - 1) / max_groups);
        m_cow_group_size_bytes = m_cow_group_size * m_scsi_block_size_cow;

        // Calculate actual number of groups needed
        m_cow_group_count = (total_sectors + m_cow_group_size - 1) / m_cow_group_size;

        // This should never happen due to our group size calculation
        assert(m_cow_group_count <= max_groups);

        m_bitmap_size = (m_cow_group_count + 7) / 8;

        // Allocate and initialize bitmap using the provided bitmap_size
        //  On RP2040, 'new uint8_t[]' returns a pointer even when the allocation failed
        // m_cow_bitmap = new uint8_t[m_bitmap_size];
        m_cow_bitmap = (uint8_t *)malloc( m_bitmap_size );

        if (!m_cow_bitmap)
        {
            //  Out of memory
            if (bitmap_max_size < 128)
            {
                logmsg("---- COW initialization failed: memory too low");
                return false;
            }

            // Try again with smaller bitmap size
            bitmap_max_size /= 2;
            logmsg("---- COW bitmap allocation of ", (int)m_bitmap_size, " bytes failed, trying max size of ", (int)bitmap_max_size, " bytes");
        }
        else
        {
            memset(m_cow_bitmap, 0, m_bitmap_size);
        }
    } while (!m_cow_bitmap);

    logmsg("---- COW image size: ", (int)image_size_bytes, " bytes");
    logmsg("---- COW bitmap: ", (int)m_cow_group_count, " groups, ", (int)m_bitmap_size, " bytes (requested: ", (int)bitmap_max_size, ")");
    logmsg("---- COW group size: ", (int)m_cow_group_size, " sectors (", (int)m_cow_group_size_bytes, " bytes)");
    logmsg("---- COW block size: ", (int)m_scsi_block_size_cow, " bytes");
    logmsg("---- COW buffer size: ", (int)g_cow_buffer_size, " bytes");

    return true;
}

// Cleanup COW resources
void COWStorage::cleanup()
{
    if (m_cow_bitmap)
    {
        // delete[] m_cow_bitmap;
        free( m_cow_bitmap );
        m_cow_bitmap = nullptr;
    }
    if (m_fsfile.isOpen())
    {
        m_fsfile.close();
    }
    if (m_fsfile_dirty.isOpen())
    {
        m_fsfile_dirty.close();
    }
}

//	Functions here are redirected ImageBackingStore functions for COW images

uint64_t COWStorage::position() const
{
	return m_current_position_cow;
}

void COWStorage::set_position(uint64_t pos)
{
    m_current_position_cow = pos;
}

void COWStorage::flush()
{
	m_fsfile_dirty.flush();
}

bool COWStorage::isOpen()
{
	return m_fsfile.isOpen();
}

uint64_t COWStorage::size()
{
	return m_fsfile.size();
}

bool COWStorage::seek( uint64_t pos )
{
	m_current_position_cow = pos;
	return true;
}

// Wrapper for cow_read that uses current file position and updates it
ssize_t COWStorage::read(void *buf, size_t count)
{
    uint32_t from = static_cast<uint32_t>(m_current_position_cow);
    uint32_t to = from + static_cast<uint32_t>(count);

    ssize_t bytes_read = cow_read(from, to, buf);

    // Update current position
    if (bytes_read > 0)
    {
        set_position(m_current_position_cow + bytes_read);
    }

    return bytes_read;
}

// Public wrapper for cow_write (that uses current file position and updates it)
ssize_t COWStorage::write(const void *buf, size_t count)
{
    uint32_t from = static_cast<uint32_t>(m_current_position_cow);
    uint32_t to = from + static_cast<uint32_t>(count);

    ssize_t bytes_written = cow_write(from, to, buf);

    // Update current position (unsure if needed)
    if (bytes_written > 0)
    {
        set_position(m_current_position_cow + bytes_written);
    }

    return bytes_written;
}

//	Group management functions

// Returns whether a group is stored in original or dirty file by checking bitmap
COWStorage::COWImageType COWStorage::getGroupImageType(uint32_t group)
{
    assert(group < m_cow_group_count);
    return (m_cow_bitmap[group / 8] & (1 << (group % 8))) ? COW_IMG_TYPE_DIRTY : COW_IMG_TYPE_ORIG;
}

// Sets group type in bitmap by setting or clearing the corresponding bit
void COWStorage::setGroupImageType(uint32_t group, COWImageType type)
{
    assert(group < m_cow_group_count);
    if (type == COW_IMG_TYPE_DIRTY)
    {
        m_cow_bitmap[group / 8] |= (1 << (group % 8));
    }
    else
    {
        m_cow_bitmap[group / 8] &= ~(1 << (group % 8));
    }
}


/*
    Reads across multiple groups, switching between original and dirty files as needed

|--------|--------|--------|--------|--------|--------|--------|--------|--------|--------|--------|--------|----- Sectors (512 bytes each)
                           |                          |                          |                          |      Groups (3 sectors each)
           DIRTY           |          CLEAN           |          CLEAN           |          DIRTY           |      Group state before write
          [---------------------------------------------------------------------------------------]         |      Read 10 blocs, spanning 4 groups
          [  DIRTY READ   ] [                      CLEAN READ                   ] [  DIRTY READ   ]|        |      Underlying chunks from alternating sources
|--------|--------|--------|--------|--------|--------|--------|--------|--------|--------|--------|--------|----- Sectors (512 bytes each)

    Idea is we repeatedly create a "chunk" that extends from the current read position
    to the next transition between original and dirty, or to the end of the read request
*/
ssize_t COWStorage::cow_read(uint32_t from, uint32_t to, void *buf)
{
    ssize_t total_bytes_read = 0;
    uint8_t *buffer_ptr = static_cast<uint8_t *>(buf);
    uint32_t current_offset = from;

    while (current_offset < to)
    {
        // Find the end of the current chunk (either 'to' or where image type changes)
        uint32_t current_group = groupFromOffset(current_offset);
        COWImageType current_type = getGroupImageType(current_group);
        uint32_t chunk_end = current_offset;

        // Extend chunk while image type remains the same and we haven't reached 'to'
        while (chunk_end < to && groupFromOffset(chunk_end) < m_cow_group_count &&
               getGroupImageType(groupFromOffset(chunk_end)) == current_type)
        {
            uint32_t next_group_offset = offsetFromGroup(groupFromOffset(chunk_end) + 1);
            chunk_end = (to < next_group_offset) ? to : next_group_offset;
        }

        // Read this chunk using cow_read_single
        ssize_t bytes_read = cow_read_single(current_offset, chunk_end - current_offset, buffer_ptr);
        if (bytes_read <= 0)
            break;

        total_bytes_read += bytes_read;
        buffer_ptr += bytes_read;
        current_offset = chunk_end;
    }

    return total_bytes_read;
}

/*
    Writes data performing copy-on-write for unmodified portions at begining and end

|--------|--------|--------|--------|--------|--------|--------|--------|--------|--------|--------|----- Sectors (512 bytes each)
                  |                          |                          |                          |      Groups (3 sectors each)
  CLEAN           |          CLEAN           |          CLEAN           |          CLEAN           |      Group state before write
                  |                  [---------------------------------------------------]         |      Write 6 blocs, spanning 3 groups
                  |[ COPY...COPY...] [ WRITE...WRITE...WRITE...WRITE...WRITE...WRITE...  ] [ COPY ]|      Actions taken (1), (2), (3)
  CLEAN           |          DIRTY           |          DIRTY           |          DIRTY           |      Group market dirty after write (4)
|--------|--------|--------|--------|--------|--------|--------|--------|--------|--------|--------|----- Sectors (512 bytes each)

    Implementation follows the above pattern:
    - (1) Handle first group: if clean and write doesn't start at group beginning, copy original data
    - (2) Write the main data to dirty file
    - (3) Handle last group: if clean and write doesn't end at group end, copy original data
    - (4) Mark all affected groups as dirty
*/
ssize_t COWStorage::cow_write(uint32_t from, uint32_t to, const void *buf)
{
    size_t count = to - from;

    uint32_t first_group = groupFromOffset(from);
    uint32_t last_group = groupFromOffset(to - 1); // Last byte affected

    // Handle first group - copy-on-write if needed and write doesn't start at group beginning
    if (getGroupImageType(first_group) == COW_IMG_TYPE_ORIG)
    {
        uint32_t group_start = offsetFromGroup(first_group);
        if (from > group_start)
        {
            // Need to preserve data before the write
            ssize_t cow_result = performCopyOnWrite(group_start, from);
            if (cow_result < 0)
            {
                return cow_result; // Return COW error immediately
            }
        }
    }

    // Handle copy in the dirty file
    m_fsfile_dirty.seek(from);
    ssize_t bytes_written = m_fsfile_dirty.write(buf, count);
    if (bytes_written <= 0)
    {
        return bytes_written;
    }

    // Handle last group - copy-on-write if needed and write doesn't end at group end
    if (getGroupImageType(last_group) == COW_IMG_TYPE_ORIG)
    {
        uint32_t group_end = offsetFromGroup(last_group + 1);
        if (to < group_end)
        {
            // Need to preserve data after the write
            ssize_t cow_result = performCopyOnWrite(to, group_end);
            if (cow_result < 0)
            {
                return cow_result; // Return COW error immediately
            }
        }
    }

    // Mark all affected groups as dirty
    for (uint32_t group = first_group; group <= last_group; ++group)
    {
        setGroupImageType(group, COW_IMG_TYPE_DIRTY);
    }

    return bytes_written;
}

// Helper for cow_read
// Reads from a single image type (original or dirty) for given byte range
// Used for implementation the high-level read
ssize_t COWStorage::cow_read_single(uint32_t from, uint32_t count, void *buf)
{
    if (getGroupImageType(groupFromOffset(from)) == COW_IMG_TYPE_DIRTY)
    {
        // Read from overlay/dirty file at same offset as original
        m_fsfile_dirty.seek(from);
        return m_fsfile_dirty.read(buf, count);
    }
    // Read from original file
    m_fsfile.seek(from);
    return m_fsfile.read(buf, count);
}

//  Helper function for cow_write
//  Copies original data to overlay (dirty) file for a specific byte range
//  Request never spans multiple groups
ssize_t COWStorage::performCopyOnWrite(uint32_t from_offset, uint32_t to_offset)
{
    // Verify both offsets are in the same group
    assert(groupFromOffset(from_offset) == groupFromOffset(to_offset - 1));

    uint32_t bytes_to_copy = to_offset - from_offset;
    uint32_t bytes_copied = 0;

    m_fsfile.seek(from_offset);
    m_fsfile_dirty.seek(from_offset);

    while (bytes_copied < bytes_to_copy)
    {
        uint32_t chunk_size = (g_cow_buffer_size < (bytes_to_copy - bytes_copied)) ? g_cow_buffer_size : (bytes_to_copy - bytes_copied);

        ssize_t bytes_read = m_fsfile.read(g_cow_buffer, chunk_size);
        if (bytes_read < 0)
        {
            logmsg("COW read error during copy-on-write operation");
            return bytes_read; // Return read error immediately
        }
        if (static_cast<uint32_t>(bytes_read) != chunk_size)
        {
            logmsg("COW unexpected partial read during copy-on-write operation");
            return -1; // Unexpected partial read
        }

        ssize_t bytes_written = m_fsfile_dirty.write(g_cow_buffer, chunk_size);
        if (bytes_written < 0)
        {
            logmsg("COW write error during copy-on-write operation");
            return bytes_written; // Return write error immediately
        }
        if (static_cast<uint32_t>(bytes_written) != chunk_size)
        {
            logmsg("COW unexpected partial write during copy-on-write operation");
            return -1; // Unexpected partial write
        }

        bytes_copied += chunk_size;
    }

    return bytes_to_copy; // Return total bytes copied
}

#endif
