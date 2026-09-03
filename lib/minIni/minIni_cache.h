// Custom .ini file access caching layer for minIni.
// This reduces boot delay by only reading the ini file once
// after boot or SD-card removal.

#pragma once

#include <stdint.h>

// Called each time an .ini file that does not fit in the cache is opened
typedef void (*ini_keep_alive_callback_t)(void);

// Set a callback to be run while an .ini file is read from the SD card because
// it does not fit in the cache. Reading a large .ini file that way takes one
// full scan of the file per setting, which can run longer than the watchdog
// allows even though progress is being made. Pass nullptr to remove the callback.
void set_ini_keep_alive_callback(ini_keep_alive_callback_t callback);

void invalidate_ini_cache();

// Note: filename must be statically allocated, pointer is stored.
void reload_ini_cache(const char *filename);

// Get the length in bytes of the .ini file as stored on the SD card.
// Returns 0 if the file has not been read, or if the cache is disabled.
uint32_t get_ini_file_length();

// Get the length in bytes of the cached copy of the .ini file.
// This is smaller than the file length because comments, blank lines and
// surrounding whitespace are stripped when the file is cached.
// Returns 0 if the file is not cached and is read from the SD card instead.
uint32_t get_ini_cache_length();

// Get the total space in bytes available for caching the .ini file.
// Returns 0 if the cache is disabled.
uint32_t get_ini_cache_capacity();

// Check whether the .ini file is currently served from the cache.
// When false, minIni reads the file from the SD card on every access.
bool is_ini_cached();

// Write the cached copy of the .ini file to 'filename' on the SD card.
// Returns false if nothing is cached, or if the file could not be written.
bool dump_ini_cache(const char *filename);
