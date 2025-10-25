# Copy-on-Write (COW) Implementation in ZuluSCSI

## Overview

The Copy-on-Write (COW) feature implementation in ZuluSCSI firmware, allows read-only disk images to appear writable to the host system while preserving the original image file integrity. This is useful for settings where a machine is used for demonstration purposes.

## Feature Description

### What is Copy-on-Write?

Copy-on-Write is a storage optimization technique where:
- The original disk image remains untouched and read-only
- All write operations are redirected to a separate "dirty" file
- Reads seamlessly combine data from both the original and dirty files
- At each cold boot, the disk image resets to its original state

### User Interface

To enable COW mode, simply rename your disk image file to have a `.cow` extension:
- `mydisk.img` → `mydisk.cow`
- ZuluSCSI automatically creates a companion `mydisk.tmp` file for writes
- The host system sees a fully writable disk
- The original data in `mydisk.cow` is never modified

#### Configuration Integration
COW settings are integrated into the existing configuration system:

**System Settings** (`zuluscsi.ini`):
```ini
[SCSI]
CowBufferSize=4096    # Buffer size for copy operations
```

To perform I/O operations while the main SCSI buffer is used, COW allocated a separate I/O buffer shared by all instances. Its default size is 4096 and is allocated only if COW is used and is never deallocated.

**Device Settings** (per-device sections):
```ini
[SCSI2]
CowBitmapSize=4096    # Bitmap size in bytes (affects granularity)
```

The file is divided in groups of file-size/(CowBitmapSize*8) bytes.
There is a bitmap for every COW device that maintains the clean/dirty size of every group.
A larger bitmap size gives smaller groups, which enhance write performance 
If the device does not have enough free memory to allocate the CowBitMapSize, its size if halved until it fits.

## Technical Implementation

### Optional feature

The COW feature is included if ENABLE_COW is set to 1. It is set to one by default, apart from the ZuluSCSIv1_1_plus platform where ENABLE_COW is set to 0 due to lack of memory.

### Architecture Overview

The COW implementation is built into the `ImageBackingStore` class with minimal impact on existing code:

```
Host SCSI Commands
       ↓
ImageBackingStore 
       ↓
┌─────────────────────────┐
│   COW Mode Enabled?     │
│  (filename ends .cow)   │
└─────────────────────────┘
       ↓
┌─────────────────────────┐    ┌─────────────────────────┐
│    Read Operation       │    │   Write Operation       │
│                         │    │                         │
│ Check bitmap for each   │    │ 1. Copy-on-write if     │
│ group:                  │    │    group is clean       │
│ • CLEAN → read original │    │ 2. Write to dirty file  │
│ • DIRTY → read dirty    │    │ 3. Mark group as dirty  │
└─────────────────────────┘    └─────────────────────────┘
```

### Core Components

#### 1. Bitmap Management
- **Purpose**: Track which disk regions have been modified
- **Granularity**: Configurable groups of sectors (default calculated from bitmap size)
- **Storage**: Bit array where 1 = dirty, 0 = clean
- **Size**: Configurable via `CowBitmapSize` setting (default 4096 bytes = 32768 groups)

#### 2. Group-Based Tracking
- **Group Size**: Dynamically calculated: `total_sectors / (bitmap_size * 8)`
- **Alignment**: Operations must be sector-aligned
- **Efficiency**: Larger groups = less memory, more over-write; smaller groups = more memory, less over-write

#### 3. File Management
- **Original File**: Opened read-only, never modified
- **Dirty File**: Created as an empty file only if doesn't already exists, same size as original
- **Automatic Creation**: Dirty file auto-created if missing or too small

### Integration Points

#### Constructor Detection
```cpp
// In ImageBackingStore constructor
if (len > 4 && strcasecmp(filename + len - 4, ".cow") == 0) {
    // Generate dirty filename by replacing .cow with .tmp
    // Initialize COW mode
}
```

#### Transparent Operation Override
All standard `ImageBackingStore` operations automatically route through COW when enabled:
- `read()` → `cow_read()`
- `write()` → `cow_write()`
- `seek()` → COW position tracking
- `isWritable()` → Always true for COW mode

### Memory Management Changes

#### The Problem
The original code relied on ImageBackingStore being trivialy copiable, which it isn't since it contains the bitmap pointer.

#### The Solution
**Placement New Pattern**: To avoid re-setting every variable by hands, which is error prone, we use destructor + placement new:

```cpp
void image_config_t::clear() {
    this->~image_config_t();      // Proper cleanup
    new (this) image_config_t();  // Fresh construction
}
```
#### Copy/Move Operations Elimination
```cpp
class ImageBackingStore {
    // All copy/move operations completely disabled
    ImageBackingStore(const ImageBackingStore&) = delete;
    ImageBackingStore& operator=(const ImageBackingStore&) = delete;
    ImageBackingStore(ImageBackingStore&&) = delete;
    ImageBackingStore& operator=(ImageBackingStore&&) = delete;
};
```

This prevents accidental expensive copies.

### Performance Optimizations

#### 1. Shared Buffer
- Global `g_cow_buffer` shared across all COW instances
- Allocated once, reused for all copy-on-write operations
- Configurable size via `CowBufferSize` setting

#### 2. Lazy Dirty File Creation
- Dirty file created only when COW mode is activated
- Uses existing `createImageFile()` infrastructure
- Sparse file allocation for efficiency

#### 3. Statistics and Monitoring
- At creation, the size of the various components are displayed in the log
- The copy-on-write overhead is tracked and displayed in the log every 1Mb written

## Configuration Reference

### System-Wide Settings
| Setting | Default | Description |
|---------|---------|-------------|
| `CowBufferSize` | 4096 | Buffer size for copy operations (bytes) |

### Per-Device Settings
| Setting | Default | Description |
|---------|---------|-------------|
| `CowBitmapSize` | 4096 | Bitmap size in bytes (affects granularity) |

