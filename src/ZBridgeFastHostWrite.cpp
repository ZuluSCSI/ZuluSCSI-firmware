/*
 * ZBridge accelerated SCSI initiator DATA OUT for the ZuluSCSI Blaster.
 *
 * The upstream initiator writes one byte at a time from the CPU. Direct RAM
 * PCM is both large and already parity-safe, so a dedicated PIO state machine
 * can perform the asynchronous REQ/ACK handshake while DMA feeds pre-expanded
 * data/parity words. The CPU remains available to service TinyUSB, which is
 * what permits the next PCM frame to arrive while this one reaches the Akai.
 */

#include "ZBridgeFastHostWrite.h"

#include <hardware/dma.h>
#include <hardware/gpio.h>
#include <hardware/pio.h>
#include <pico/platform.h>
#include <pico/time.h>

#include "ZBridge.h"
#include "ZuluSCSI_platform.h"

extern "C" {
#include <scsi.h>
}

#if defined(ZBRIDGE_DIRECT_RAM_MODE) && defined(PLATFORM_HAS_INITIATOR_MODE) && defined(ZULUSCSI_BLASTER)

namespace {
PIO kPIO = pio0;
constexpr uint kStateMachine = 0;
// Expand parity in bounded 256-byte DMA windows. Larger scratch buffers do
// not increase SCSI throughput and unnecessarily consume RP2350 SRAM.
constexpr uint32_t kParityWords = 256;
constexpr uint32_t kMinimumAcceleratedBytes = 512;
// Pull a complete protocol credit out of the two-slot USB ring before sending
// its first PIO window. That returns the slot to the Mac while these 4 KiB are
// still being clocked into the sampler, leaving one prefetched block plus the
// other ring slot as refill runway instead of only the final 256-byte window.
constexpr uint32_t kStreamPrefetchBytes = 4 * 1024;
// The CPU writer holds DB0...DBP stable for about 100 ns before asserting
// ACK. The original PIO path asserted ACK on the instruction immediately
// after OUT, so the S3200XL could sample invalid data/parity and abandon the
// very first DATA OUT phase. Fifteen PIO delay cycles reproduce the proven
// setup interval while keeping the remainder of the handshake asynchronous.
constexpr uint kDataSetupDelayCycles = 15;

int g_dmaChannel = -1;
int g_programOffset = -1;
pio_sm_config g_config;
// RP2350 has two independent 4 KB scratch banks in addition to its 512 KB
// main SRAM. Keep the DMA staging window in scratch Y so enabling the fast
// writer cannot crowd the rest of ZuluSCSI's working set out of main RAM.
uint32_t g_parityBuffer[kParityWords]
    __attribute__((aligned(4), section(".scratch_y.zbridge_parity")));
// A full credit does not fit beside the parity words in scratch Y. Keep this
// bounded staging block in main SRAM; current builds retain enough linker headroom
// for it, and the added runway removes repeated USB starvation on the live bus.
uint8_t g_streamInput[kStreamPrefetchBytes] __attribute__((aligned(4)));

// pull count; then repeat { pull parity word, drive DB0...DBP, wait for the
// target's active-low REQ, pulse active-low ACK, wait for REQ release }.
// Completion is pushed to RX so the CPU never guesses whether the final word
// merely left the DMA FIFO or actually completed its SCSI handshake.
const uint16_t g_programInstructions[] = {
    uint16_t(pio_encode_pull(false, true) | pio_encode_sideset(1, 1)),
    uint16_t(pio_encode_mov(pio_x, pio_osr) | pio_encode_sideset(1, 1)),
    uint16_t(pio_encode_pull(false, true) | pio_encode_sideset(1, 1)),
    uint16_t(pio_encode_out(pio_pins, 9)
        | pio_encode_sideset(1, 1)
        | pio_encode_delay(kDataSetupDelayCycles)),
    uint16_t(pio_encode_wait_gpio(false, SCSI_IN_REQ) | pio_encode_sideset(1, 1)),
    uint16_t(pio_encode_nop() | pio_encode_sideset(1, 0)),
    uint16_t(pio_encode_wait_gpio(true, SCSI_IN_REQ) | pio_encode_sideset(1, 0)),
    uint16_t(pio_encode_jmp_x_dec(2) | pio_encode_sideset(1, 1)),
    uint16_t(pio_encode_mov(pio_isr, pio_null) | pio_encode_sideset(1, 1)),
    uint16_t(pio_encode_push(false, true) | pio_encode_sideset(1, 1)),
};

const pio_program g_program = {
    g_programInstructions,
    sizeof(g_programInstructions) / sizeof(g_programInstructions[0]),
    -1,
};

bool ensureInitialized()
{
    if (g_dmaChannel >= 0 && g_programOffset >= 0) return true;
    if (!pio_can_add_program(kPIO, &g_program)) return false;

    int channel = dma_claim_unused_channel(false);
    if (channel < 0) return false;

    g_programOffset = int(pio_add_program(kPIO, &g_program));
    g_dmaChannel = channel;
    g_config = pio_get_default_sm_config();
    sm_config_set_wrap(&g_config, uint(g_programOffset),
                       uint(g_programOffset + int(g_program.length) - 1));
    sm_config_set_out_pins(&g_config, SCSI_IO_DB0, 9);
    sm_config_set_sideset(&g_config, 1, false, false);
    sm_config_set_sideset_pins(&g_config, SCSI_OUT_ACK);
    sm_config_set_out_shift(&g_config, true, false, 32);
    return true;
}

void restoreSIO()
{
    pio_sm_set_enabled(kPIO, kStateMachine, false);
    pio_sm_clear_fifos(kPIO, kStateMachine);
    for (uint pin = SCSI_IO_DB0; pin <= SCSI_IO_DBP; ++pin)
        gpio_set_function(pin, GPIO_FUNC_SIO);
    gpio_set_function(SCSI_OUT_ACK, GPIO_FUNC_SIO);
    SCSI_RELEASE_DATA_REQ();
    SCSI_OUT(ACK, 0);
}

bool phaseStillDataOut(int cdStart, int msgStart, volatile int *resetFlag)
{
    return !*resetFlag && !SCSI_IN(IO)
        && SCSI_IN(CD) == cdStart && SCSI_IN(MSG) == msgStart;
}
} // namespace

static uint32_t runFastHostWrite(const uint8_t *data, uint32_t count,
                                 volatile int *resetFlag, bool streaming)
{
    if ((!data && !streaming) || !resetFlag || count < kMinimumAcceleratedBytes
        || !ensureInitialized()) {
        return UINT32_MAX;
    }

    const int cdStart = SCSI_IN(CD);
    const int msgStart = SCSI_IN(MSG);
    SCSI_OUT(ACK, 0);
    SCSI_ENABLE_DATA_OUT();

    pio_sm_set_enabled(kPIO, kStateMachine, false);
    pio_sm_clear_fifos(kPIO, kStateMachine);
    pio_sm_restart(kPIO, kStateMachine);
    pio_sm_init(kPIO, kStateMachine, uint(g_programOffset), &g_config);
    // Preload the PIO output latches before handing the live bus pins over
    // from SIO. Without this, SM0's reset value briefly drives ACK low during
    // the mux transition, acknowledging a byte before the first REQ/data pair
    // exists. The S3200XL then abandons DATA OUT even though the queued USB
    // frames themselves are valid.
    const uint32_t idleOutputs = SCSI_IO_DATA_MASK | (1u << SCSI_OUT_ACK);
    pio_sm_set_pins_with_mask(kPIO, kStateMachine,
                              idleOutputs, idleOutputs);
    pio_sm_set_consecutive_pindirs(kPIO, kStateMachine, SCSI_IO_DB0, 9, true);
    pio_sm_set_consecutive_pindirs(kPIO, kStateMachine, SCSI_OUT_ACK, 1, true);
    for (uint pin = SCSI_IO_DB0; pin <= SCSI_IO_DBP; ++pin)
        gpio_set_function(pin, GPIO_FUNC_PIO0);
    gpio_set_function(SCSI_OUT_ACK, GPIO_FUNC_PIO0);
    pio_sm_set_enabled(kPIO, kStateMachine, true);

    dma_channel_config dmaConfig = dma_channel_get_default_config(uint(g_dmaChannel));
    channel_config_set_transfer_data_size(&dmaConfig, DMA_SIZE_32);
    channel_config_set_read_increment(&dmaConfig, true);
    channel_config_set_write_increment(&dmaConfig, false);
    channel_config_set_dreq(&dmaConfig,
                            pio_get_dreq(kPIO, kStateMachine, true));

    uint32_t completed = 0;
    uint32_t stagedOffset = 0;
    uint32_t stagedLength = 0;
    while (completed < count) {
        const uint32_t remaining = count - completed;
        uint32_t amount = remaining < kParityWords
            ? remaining : kParityWords;

        const uint8_t *window = data ? data + completed : nullptr;
        if (streaming) {
            if (stagedOffset == stagedLength) {
                stagedOffset = 0;
                stagedLength = remaining < kStreamPrefetchBytes
                    ? remaining : kStreamPrefetchBytes;
                const uint32_t received = zbridge_stream_read(
                    g_streamInput, stagedLength);
                if (received != stagedLength) {
                    restoreSIO();
                    return completed;
                }
            }
            const uint32_t stagedAvailable = stagedLength - stagedOffset;
            if (amount > stagedAvailable) amount = stagedAvailable;
            window = g_streamInput + stagedOffset;
        }
        for (uint32_t index = 0; index < amount; ++index)
            g_parityBuffer[index] = g_scsi_parity_lookup[window[index]];

        // X is a count-minus-one loop counter. FIFO ordering guarantees this
        // word is consumed before the DMA-fed parity/data words.
        pio_sm_put_blocking(kPIO, kStateMachine, amount - 1);
        dma_channel_configure(uint(g_dmaChannel), &dmaConfig,
                              &kPIO->txf[kStateMachine], g_parityBuffer,
                              amount, true);

        const uint32_t windowStarted = time_us_32();
        while (pio_sm_is_rx_fifo_empty(kPIO, kStateMachine)) {
            zbridge_stream_pump_usb();
            platform_reset_watchdog();
            if (!phaseStillDataOut(cdStart, msgStart, resetFlag)) {
                dma_channel_abort(uint(g_dmaChannel));
                restoreSIO();
                return completed;
            }
            // scsiInitiatorRunCommand's outer timeout cannot run while this
            // accelerated writer owns DATA OUT. Bound every PIO window here
            // so a missing target handshake fails closed and produces a useful
            // firmware error instead of wedging until the Mac disconnects.
            if (uint32_t(time_us_32() - windowStarted) > 3'000'000u) {
                dma_channel_abort(uint(g_dmaChannel));
                restoreSIO();
                return completed;
            }
            tight_loop_contents();
        }
        (void)pio_sm_get(kPIO, kStateMachine);
        completed += amount;
        if (streaming) stagedOffset += amount;
    }

    while (dma_channel_is_busy(uint(g_dmaChannel))) {
        zbridge_stream_pump_usb();
        tight_loop_contents();
    }
    restoreSIO();
    return completed;
}

uint32_t zbridgeFastHostWrite(const uint8_t *data, uint32_t count,
                              volatile int *resetFlag)
{
    return runFastHostWrite(data, count, resetFlag, false);
}

uint32_t zbridgeFastHostStreamWrite(uint32_t count,
                                    volatile int *resetFlag)
{
    return runFastHostWrite(nullptr, count, resetFlag, true);
}

#else

uint32_t zbridgeFastHostWrite(const uint8_t *, uint32_t,
                              volatile int *)
{
    return UINT32_MAX;
}

uint32_t zbridgeFastHostStreamWrite(uint32_t, volatile int *)
{
    return UINT32_MAX;
}

#endif
