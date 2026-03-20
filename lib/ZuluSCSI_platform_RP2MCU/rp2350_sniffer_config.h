// Configuration for RP2350_Logic_Sniffer

#pragma once

#include "ZuluSCSI_platform.h"
#include <stdio.h>

#ifdef PLATFORM_HAS_SNIFFER

#ifdef __cplusplus
extern "C"
#endif
void sniffer_log(const char *msg);

#define SNIFFER_LOG(x, ...) do { \
    char linebuf[128]; \
    snprintf(linebuf, sizeof(linebuf), x, __VA_ARGS__); \
    sniffer_log(linebuf); \
} while (0)

// Size of capture data blocks, must be a multiple of 4 bytes.
#ifndef SNIFFER_BLOCKSIZE
#define SNIFFER_BLOCKSIZE 1024
#endif

// Number of capture data blocks to allocate
// Must be a power of two
#ifndef SNIFFER_BLOCKCOUNT
#define SNIFFER_BLOCKCOUNT 32
#endif

// DMA channel for transfer of data from PIO to RAM
#ifndef SNIFFER_DMACH
#define SNIFFER_DMACH 6
#endif

// DMA channel for reconfiguring first DMA channel
#ifndef SNIFFER_DMACH_B
#define SNIFFER_DMACH_B 7
#endif

// PIO block used for capture
#ifndef SNIFFER_PIO
#define SNIFFER_PIO pio2
#endif

// PIO state machine used for main sniffer code
#ifndef SNIFFER_PIO_SM
#define SNIFFER_PIO_SM 3
#endif

// PIO state machines for triggering
// If you have other code on the PIO, you can reduce the number of these
#ifndef SNIFFER_PIO_SM_TRIGGER_MIN
#define SNIFFER_PIO_SM_TRIGGER_MIN 0
#endif
#ifndef SNIFFER_PIO_SM_TRIGGER_MAX
#define SNIFFER_PIO_SM_TRIGGER_MAX 2
#endif

#endif
