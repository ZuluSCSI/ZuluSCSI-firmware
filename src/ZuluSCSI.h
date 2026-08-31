// Common functions and global state available to all modules

#pragma once

#include <SdFat.h>

extern bool g_sdcard_present;
extern SdFs SD;

// Create an zero-filled image file of specified size -- update UI with progress
//  If .vhd file footer fails to write renames extension to .raw
//	Returns true if image created
//	Returns false if file already exists or on error
bool createImageFile(char *imgname, uint64_t size);
