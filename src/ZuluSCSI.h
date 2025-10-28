// Common functions and global state available to all modules

#pragma once

extern bool g_sdcard_present;

// Create an zero-filled image file of specified size -- update UI with progress
//	Returns true if image created
//	Returns false if file already exists or on error
bool createImageFile(const char *imgname, uint64_t size);
