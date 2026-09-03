#pragma once

#include <stdint.h>

// Continuous RP2350 initiator DATA IN path used only while protocol v9 owns
// the native vendor USB endpoint. The SCSI command remains selected while
// bounded PIO windows are copied to USB. UINT32_MAX means unavailable.
uint32_t zbridgeFastHostStreamRead(uint32_t count, int *parityError,
                                  int busWidth, volatile int *resetFlag);
