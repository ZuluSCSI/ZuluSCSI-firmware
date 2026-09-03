/*
 * ZBridge continuous SCSI initiator DATA IN for the ZuluSCSI Blaster.
 *
 * Upstream already provides a hardware-accelerated PIO REQ/ACK reader. This
 * adapter keeps one Akai RECEIVE(6) selected, drains it into a bounded aligned
 * staging window, and gives each window to TinyUSB before acknowledging more
 * target bytes. A target may hold REQ while USB drains; no sampler addressing
 * or command semantics are changed.
 */

#include "ZBridgeFastHostRead.h"

#include <stdint.h>

#include "ZBridge.h"
#include "ZuluSCSI_platform.h"
#include "scsi_accel_host.h"

#if defined(ZBRIDGE_DIRECT_RAM_MODE) && defined(PLATFORM_HAS_INITIATOR_MODE) && defined(ZULUSCSI_BLASTER)

namespace {
// One full TinyUSB controller window is enough to overlap the ~1 MB/s USB
// lane with the slower sampler bus, while retaining safe RP2350 heap margin.
constexpr uint32_t kReadWindowBytes = 1024;
constexpr uint32_t kMinimumAcceleratedBytes = 512;
uint8_t g_readWindow[kReadWindowBytes] __attribute__((aligned(4)));
}

uint32_t zbridgeFastHostStreamRead(uint32_t count, int *parityError,
                                  int busWidth, volatile int *resetFlag)
{
    if (!resetFlag || count < kMinimumAcceleratedBytes || (count & 1u))
        return UINT32_MAX;

    uint32_t completed = 0;
    while (completed < count)
    {
        uint32_t amount = count - completed;
        if (amount > kReadWindowBytes) amount = kReadWindowBytes;

        const uint32_t received = scsi_accel_host_read(
            g_readWindow, amount, parityError, busWidth, resetFlag);
        if (received != amount) return completed + received;
        if (!zbridge_stream_write(g_readWindow, amount)) return completed;
        completed += amount;
    }
    return completed;
}

#else

uint32_t zbridgeFastHostStreamRead(uint32_t, int *, int, volatile int *)
{
    return UINT32_MAX;
}

#endif
