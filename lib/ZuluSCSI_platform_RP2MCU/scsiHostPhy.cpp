/** 
 * ZuluSCSI™ - Copyright (c) 2022-2025 Rabbit Hole Computing™
 * 
 * ZuluSCSI™ firmware is licensed under the GPL version 3 or any later version. 
 * 
 * https://www.gnu.org/licenses/gpl-3.0.html
 * ----
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version. 
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details. 
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
**/

#include "scsiHostPhy.h"
#include "ZuluSCSI_platform.h"
#include "ZuluSCSI_log.h"
#include "ZuluSCSI_log_trace.h"
#include "scsi_accel_host.h"
#if defined(ZBRIDGE_DIRECT_RAM_MODE)
#include "ZBridge.h"
#include "ZBridgeFastHostWrite.h"
#include "ZBridgeFastHostRead.h"
#endif
#include <assert.h>

#include <scsi2sd.h>
extern "C" {
#include <scsi.h>
}

volatile int g_scsiHostPhyReset;
int g_scsiHostBusWidth;
uint8_t g_scsiHostPhySelectionStage;
uint32_t g_scsiHostPhySelectionSignals;
static bool g_scsiHostPhySawRequest;

#ifndef PLATFORM_HAS_INITIATOR_MODE

// Dummy functions for platforms without hardware support for
// SCSI initiator mode.
void scsiHostPhyReset(void) {}
void scsiHostPhyActivateNoReset(void) {}
void scsiHostPhyDeactivate(void) {}
bool scsiHostPhySelect(int target_id, uint8_t initiator_id, bool request_atn) { return false; }
uint32_t scsiHostPhyGetSignals(void) { return 0; }
uint32_t scsiHostPhyGetGPIOState(void) { return 0; }
int scsiHostPhyGetPhase() { return 0; }
bool scsiHostRequestWaiting() { return false; }
uint32_t scsiHostWrite(const uint8_t *data, uint32_t count) { return 0; }
uint32_t scsiHostRead(uint8_t *data, uint32_t count) { return 0; }
void scsiHostPhyRelease();

#else

// Release bus and pulse RST signal, initialize PHY to host mode.
void scsiHostPhyReset(void)
{
    SCSI_RELEASE_OUTPUTS();
    SCSI_ENABLE_INITIATOR();

    scsi_accel_host_init();

    SCSI_OUT(RST, 1);
    delay(2);
    SCSI_OUT(RST, 0);
    delay(250);
    g_scsiHostPhyReset = false;
}

void scsiHostPhyActivateNoReset(void)
{
    SCSI_RELEASE_OUTPUTS();

    // A live ZBridge role handoff starts from target-mode GPIO settings.
    // Recreate the pin configuration used by a Blaster that booted directly
    // in initiator mode; changing OE bits alone leaves ACK/ATN with target-mode
    // slew and several phase inputs without the initiator pull configuration.
    const uint input_pins[] = {
        SCSI_IN_IO, SCSI_IN_MSG, SCSI_IN_CD, SCSI_IN_REQ,
        SCSI_IN_BSY, SCSI_IN_RST
    };
    for (uint pin : input_pins)
    {
        gpio_set_function(pin, GPIO_FUNC_SIO);
        gpio_set_pulls(pin, true, false);
        gpio_set_dir(pin, GPIO_IN);
        gpio_set_slew_rate(pin, GPIO_SLEW_RATE_SLOW);
    }

    const uint output_pins[] = {
        SCSI_OUT_RST, SCSI_OUT_SEL, SCSI_OUT_ACK, SCSI_OUT_ATN
    };
    for (uint pin : output_pins)
    {
        gpio_set_function(pin, GPIO_FUNC_SIO);
        gpio_set_pulls(pin, false, false);
        gpio_put(pin, 1); // SCSI controls are active low.
        gpio_set_dir(pin, GPIO_OUT);
        gpio_set_slew_rate(pin, GPIO_SLEW_RATE_FAST);
    }

    SCSI_ENABLE_INITIATOR();
    scsi_accel_host_init();
    g_scsiHostPhyReset = false;
}

void scsiHostPhyDeactivate(void)
{
    SCSI_RELEASE_OUTPUTS();
    gpio_set_dir(SCSI_OUT_ACK, GPIO_IN);
    gpio_set_dir(SCSI_OUT_ATN, GPIO_IN);
}

uint32_t scsiHostPhyGetSignals(void)
{
    uint32_t signals = 0;
    if (SCSI_IN(BSY)) signals |= 1u << 0;
    if (SCSI_IN(SEL)) signals |= 1u << 1;
    if (SCSI_IN(REQ)) signals |= 1u << 2;
    if (SCSI_IN(CD))  signals |= 1u << 3;
    if (SCSI_IN(IO))  signals |= 1u << 4;
    if (SCSI_IN(MSG)) signals |= 1u << 5;
    if (SCSI_IN(ATN)) signals |= 1u << 6;
    signals |= (SCSI_IN_DATA() & 0xFFu) << 8;
    return signals;
}

uint32_t scsiHostPhyGetGPIOState(void)
{
    const uint pins[] = {
        SCSI_IN_REQ, SCSI_IN_CD, SCSI_IN_MSG, SCSI_IN_IO,
        SCSI_IN_ATN, SCSI_IN_ACK, SCSI_OUT_SEL, SCSI_OUT_BSY, SCSI_OUT_RST
    };
    uint32_t directions = 0;
    uint32_t inputs = 0;
    uint32_t outputs = 0;
    for (uint bit = 0; bit < sizeof(pins) / sizeof(pins[0]); ++bit)
    {
        const uint pin = pins[bit];
        if (gpio_get_dir(pin) == GPIO_OUT) directions |= 1u << bit;
        if (gpio_get(pin)) inputs |= 1u << bit;
        if (gpio_get_out_level(pin)) outputs |= 1u << bit;
    }
    return directions | (inputs << 9) | (outputs << 18);
}

// Select a device and an initiator, ids 0-7.
// Returns true if the target answers to selection request.
bool scsiHostPhySelect(int target_id, uint8_t initiator_id, bool request_atn)
{
    g_scsiHostPhySelectionStage = 0;
    g_scsiHostPhySelectionSignals = 0;
    g_scsiHostPhySawRequest = false;

    // Command phase always happens in 8-bit mode
    scsiHostSetBusWidth(0);

    // Claim the first electrically free sample directly from this selection
    // routine. The Akai continuously polls HD5; build 12's separate stability
    // timer could expire even though its final raw sample showed BSY/SEL free.
    uint32_t busFreeWaitStarted = millis();
    uint32_t lastWatchdogReset = busFreeWaitStarted;
    while (true)
    {
        // SCSI requires both lines to remain released for the bus-free delay,
        // not merely to be observed free in one sample. This matters on the
        // multi-initiator Akai bus, where ID 6 can begin another HD poll in the
        // same interval that computer/initiator ID 7 is trying to arbitrate.
        if (!SCSI_IN(BSY) && !SCSI_IN(SEL))
        {
            delayMicroseconds(1); // >= 800 ns SCSI bus-free delay
            if (!SCSI_IN(BSY) && !SCSI_IN(SEL)) break;
        }

        uint32_t now = millis();
        if ((uint32_t)(now - busFreeWaitStarted) >= 5000)
        {
            g_scsiHostPhySelectionSignals = scsiHostPhyGetSignals();
            dbgmsg("scsiHostPhySelect: bus never became free");
            return false;
        }

        // The Akai can immediately reselect its configured disk target after
        // releasing the bus. platform_poll() is much longer than that free
        // interval, so build 18 repeatedly slept through the only opportunity
        // to arbitrate. Keep this loop time-critical and service only the MCU
        // watchdog until we have claimed the bus.
        if ((uint32_t)(now - lastWatchdogReset) >= 100)
        {
            platform_reset_watchdog();
            lastWatchdogReset = now;
        }
        delayMicroseconds(1);
    }

    g_scsiHostPhySelectionStage = 1;

    if (!request_atn)
    {
        // Preserve the exact modified-arbitration sequence used by the
        // upstream Blaster initiator implementation. The S3200XL processor
        // acknowledges a target-only ID selection but does not subsequently
        // assert REQ; it needs to see both processor ID 6 and computer ID 7.
        scsiLogInitiatorPhaseChange(BUS_BUSY);
        SCSI_OUT(BSY, 1);
        for (int wait = 0; wait < 10; wait++)
        {
            delayMicroseconds(1);
#ifdef ZULUSCSI_WIDE
            if (SCSI_IN_DATA() == 0)
#else
            if (SCSI_IN_DATA() != 0)
#endif
            {
                g_scsiHostPhySelectionSignals = scsiHostPhyGetSignals();
                SCSI_RELEASE_OUTPUTS();
                return false;
            }
        }

        scsiLogInitiatorPhaseChange(SELECTION);
        dbgmsg("------ BLASTER SELECTING ", target_id,
               " with initiator ID ", (int)initiator_id);
        SCSI_OUT(SEL, 1);
        delayMicroseconds(5);
        SCSI_OUT_DATA((1 << target_id) | (1 << initiator_id));
        delayMicroseconds(5);
        g_scsiHostPhySelectionStage = 2;
        SCSI_OUT(BSY, 0);
    }
    else
    {
        // Standards-style Macintosh/SCSI2Pi arbitration with ATN/IDENTIFY.
        scsiLogInitiatorPhaseChange(BUS_BUSY);
        SCSI_OUT_DATA(1 << initiator_id);
        delayMicroseconds(1);
        SCSI_OUT(BSY, 1);
        delayMicroseconds(3); // >= 2.4 us SCSI arbitration delay

        scsiLogInitiatorPhaseChange(SELECTION);
        dbgmsg("------ SCSI-2 SELECTING ", target_id, " with initiator ID ", (int)initiator_id);
        SCSI_OUT(SEL, 1);
        delayMicroseconds(2); // bus-clear + bus-settle delays
        SCSI_OUT_DATA((1 << target_id) | (1 << initiator_id));
        delayMicroseconds(1);
        SCSI_OUT(ATN, 1);
        delayMicroseconds(1); // two deskew delays
        g_scsiHostPhySelectionStage = 2;
        SCSI_OUT(BSY, 0);
        delayMicroseconds(1); // bus-settle delay
    }

    // Wait up to the standard 250 ms selection timeout, but sample at 1 us
    // intervals. Once the target asserts BSY, the initiator must release its
    // selection data and SEL promptly. Logging or 100 us polling here left the
    // S3200XL waiting hundreds of microseconds and stranded it before COMMAND.
    uint32_t selectionWaitStarted = micros();
    while (!SCSI_IN(BSY) &&
           (uint32_t)(micros() - selectionWaitStarted) < 250000)
    {
        delayMicroseconds(1);
    }

    if (!SCSI_IN(BSY))
    {
        // No response
        g_scsiHostPhySelectionSignals = scsiHostPhyGetSignals();
        dbgmsg("ZBridge selection timeout stage ", (int)g_scsiHostPhySelectionStage,
               " signals ", (uint32_t)g_scsiHostPhySelectionSignals);
        SCSI_RELEASE_OUTPUTS();
        return false;
    }

    g_scsiHostPhySelectionStage = 3;

    // Preserve the exact Blaster handoff order from upstream initiator mode:
    // release the selection IDs, enable U105's status inputs, then release
    // SEL. On this board the control pins are multiplexed, so reordering these
    // operations can lose the target's first REQ transition.
    SCSI_RELEASE_DATA_REQ();
    SCSI_OUT(BSY, 1);
    SCSI_OUT(SEL, 0);
    g_scsiHostPhySelectionStage = 4;
    return true;
}

void scsiHostPhySetATN(bool atn)
{
    SCSI_OUT(ATN, atn);
}

void scsiHostSetBusWidth(int busWidth)
{
#ifdef ZULUSCSI_WIDE
    g_scsiHostBusWidth = busWidth;
#else
    assert(busWidth == 0);
#endif
}

// Read the current communication phase as signaled by the target
int scsiHostPhyGetPhase()
{
    static absolute_time_t last_online_time;

    if (g_scsiHostPhyReset)
    {
        // Reset request from watchdog timer
        scsiHostPhyRelease();
        return BUS_FREE;
    }

    bool req_in = SCSI_IN(REQ);
    if (req_in) g_scsiHostPhySawRequest = true;

    int phase = 0;
    if (SCSI_IN(CD)) phase |= __scsiphase_cd;
    if (SCSI_IN(IO)) phase |= __scsiphase_io;
    if (SCSI_IN(MSG)) phase |= __scsiphase_msg;

    // OUT_BSY enables the Blaster's status-input buffer after selection. Only
    // release it to sample the target's real BSY when no target-driven phase
    // lines are visible. Build 20 pulsed BSY whenever REQ was momentarily
    // inactive, including while the Akai was establishing MESSAGE OUT or
    // COMMAND, which caused the processor target to abandon the selection.
    if (g_scsiHostPhySawRequest && !req_in && phase == 0 &&
        absolute_time_diff_us(last_online_time, get_absolute_time()) > 100)
    {
        // Disable OUT_BSY for a short time to see if the target is still on line
        SCSI_OUT(BSY, 0);
        delayMicroseconds(1);

        if (!SCSI_IN(BSY))
        {
            scsiLogInitiatorPhaseChange(BUS_FREE);
            return BUS_FREE;
        }

        // Still online, re-enable OUT_BSY to enable IO buffers
        SCSI_OUT(BSY, 1);
        last_online_time = get_absolute_time();
    }
    else if (phase != 0)
    {
        last_online_time = get_absolute_time();
    }

    if (!req_in)
    {
        // The Blaster multiplexes selection and phase inputs. Before REQ, its
        // MSG/BSY GPIO can contain the target's selection BSY state rather
        // than a valid MSG phase. Only trust CD/IO/MSG after REQ is asserted.
        return BUS_BUSY;
    }

    scsiLogInitiatorPhaseChange(phase);
    return phase;
}

bool scsiHostRequestWaiting()
{
    return SCSI_IN(REQ);
}

// Blocking data transfer
#define SCSIHOST_WAIT_ACTIVE(pin) \
  if (!SCSI_IN(pin)) { \
    if (!SCSI_IN(pin)) { \
      while(!SCSI_IN(pin) && !g_scsiHostPhyReset); \
    } \
  }

#define SCSIHOST_WAIT_INACTIVE(pin) \
  if (SCSI_IN(pin)) { \
    if (SCSI_IN(pin)) { \
      while(SCSI_IN(pin) && !g_scsiHostPhyReset); \
    } \
  }

// Write one byte to SCSI target using the handshake mechanism
static inline void scsiHostWriteOneByte(uint8_t value)
{
    SCSIHOST_WAIT_ACTIVE(REQ);
    SCSI_OUT_DATA(value);
    delay_100ns(); // DB setup time before ACK
    SCSI_OUT(ACK, 1);
    SCSIHOST_WAIT_INACTIVE(REQ);
    SCSI_RELEASE_DATA_REQ();
    SCSI_OUT(ACK, 0);
}

// Read one byte from SCSI target using the handshake mechanism.
static inline uint8_t scsiHostReadOneByte(int* parityError)
{
    SCSIHOST_WAIT_ACTIVE(REQ);
    uint32_t r = SCSI_IN_DATA();
    SCSI_OUT(ACK, 1);
    SCSIHOST_WAIT_INACTIVE(REQ);
    SCSI_OUT(ACK, 0);

#ifdef ZULUSCSI_WIDE // wide keeps data signals inverted
    if (parityError && !scsi_check_parity(~r))
#else
    if (parityError && !scsi_check_parity(r))
#endif
    {

        logmsg("Parity error in scsiReadOneByte(): ", (uint32_t)r);
        *parityError = 1;
    }

    return (uint8_t)r;
}

#ifdef ZULUSCSI_WIDE
static inline void scsiHostWriteOneWord(uint16_t value)
{
    SCSIHOST_WAIT_ACTIVE(REQ);
    SCSI_OUT_DATA(value);
    delay_100ns(); // DB setup time before ACK
    SCSI_OUT(ACK, 1);
    SCSIHOST_WAIT_INACTIVE(REQ);
    SCSI_RELEASE_DATA_REQ();
    SCSI_OUT(ACK, 0);
}

// Read one byte from SCSI target using the handshake mechanism.
static inline uint16_t scsiHostReadOneWord(int* parityError)
{
    SCSIHOST_WAIT_ACTIVE(REQ);
    uint32_t r = SCSI_IN_DATA();
    SCSI_OUT(ACK, 1);
    SCSIHOST_WAIT_INACTIVE(REQ);
    SCSI_OUT(ACK, 0);

    if (parityError && !scsi_check_parity_16bit(~r))
    {
        logmsg("Parity error in scsiHostReadOneWord(): ", (uint32_t)r);
        *parityError = 1;
    }

    return (uint16_t)r;
}
#endif

uint32_t scsiHostWrite(const uint8_t *data, uint32_t count)
{
#if defined(ZBRIDGE_DIRECT_RAM_MODE)
    // Protocol v7 starts one Akai SEND command for an entire PCM window and
    // feeds its DATA OUT phase from the USB-owned ring. Bypass logging here:
    // `data` is only a non-null initiator sentinel, not a `count`-byte buffer.
    if (zbridge_streaming_write_active() && count >= 512)
    {
        return zbridgeFastHostStreamWrite(count, &g_scsiHostPhyReset);
    }
#endif

    scsiLogDataOut(data, count);

#if defined(ZBRIDGE_DIRECT_RAM_MODE)
    // Legacy/fallback Direct-RAM PCM blocks use the pointer-backed PIO/DMA
    // writer. Small command and SysEx payloads stay on the upstream byte path.
    const uint32_t accelerated = zbridgeFastHostWrite(
        data, count, &g_scsiHostPhyReset);
    if (accelerated != UINT32_MAX)
    {
        return accelerated;
    }
#endif

    int cd_start = SCSI_IN(CD);
    int msg_start = SCSI_IN(MSG);

    for (uint32_t i = 0; i < count; i++)
    {
        while (!SCSI_IN(REQ))
        {
            if (g_scsiHostPhyReset || SCSI_IN(IO) || SCSI_IN(CD) != cd_start || SCSI_IN(MSG) != msg_start)
            {
                // Target switched out of DATA_OUT mode
                logmsg("scsiHostWrite: sent ", (int)i, " bytes, expected ", (int)count);
                return i;
            }
        }

        if (g_scsiHostBusWidth == 0)
        {
            scsiHostWriteOneByte(data[i]);
        }
#ifdef ZULUSCSI_WIDE
        else if (g_scsiHostBusWidth == 1)
        {
            uint16_t word = data[i++];
            if (i < count) word |= (uint16_t)data[i] << 8;
            scsiHostWriteOneWord(word);
        }
#endif
        else
        {
            logmsg("Invalid bus width ", g_scsiHostBusWidth);
            return 0;
        }
    }

    return count;
}

uint32_t scsiHostRead(uint8_t *data, uint32_t count)
{
    int parityErrorValue = 0;
    int* parityError = nullptr;
    if (g_scsi_settings.getSystem()->initiatorParity)
    {
        parityError = &parityErrorValue;
    }
    uint32_t fullcount = count;

    int cd_start = SCSI_IN(CD);
    int msg_start = SCSI_IN(MSG);

#if defined(ZBRIDGE_DIRECT_RAM_MODE)
    // Protocol v9 uses a non-null sentinel for a continuous Akai RECEIVE(6)
    // that is much larger than the command parser buffer. Stream PIO DATA IN
    // directly to the vendor USB endpoint and never log/dereference sentinel
    // bytes through the ordinary pointer-backed path.
    if (zbridge_streaming_read_active() && count >= 512)
    {
        count = zbridgeFastHostStreamRead(
            count, parityError, g_scsiHostBusWidth, &g_scsiHostPhyReset);
        if (count == UINT32_MAX || g_scsiHostPhyReset
            || (parityError && *parityError))
        {
            return 0;
        }
        return count;
    }
#endif

    if ((count & 1) == 0 && ((uint32_t)data & 1) == 0)
    {
        // Even number of bytes, use accelerated routine
        count = scsi_accel_host_read(data, count, parityError, g_scsiHostBusWidth, &g_scsiHostPhyReset);
    }
    else
    {
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t start = millis();
            while (!SCSI_IN(REQ) && (millis() - start) < 10000)
            {
                // Wait for REQ asserted
            }

            int io = SCSI_IN(IO);
            int cd = SCSI_IN(CD);
            int msg = SCSI_IN(MSG);

            if (g_scsiHostPhyReset)
            {
                dbgmsg("sciHostRead: aborting due to reset request");
                count = i;
                break;
            }
            else if (!io || cd != cd_start || msg != msg_start)
            {
                dbgmsg("scsiHostRead: aborting because target switched transfer phase (IO: ", io, ", CD: ", cd, ", MSG: ", msg, ")");
                count = i;
                break;
            }

            if (g_scsiHostBusWidth == 0)
            {
                data[i] = scsiHostReadOneByte(parityError);
            }
#ifdef ZULUSCSI_WIDE
            else if (g_scsiHostBusWidth == 1)
            {
                uint16_t word = scsiHostReadOneWord(parityError);
                data[i++] = word & 0xFF;
                if (i < count) data[i] = word >> 8;
            }
#endif
            else
            {
                logmsg("Invalid bus width ", g_scsiHostBusWidth);
                return 0;
            }
        }
    }

    scsiLogDataIn(data, count);

    if (g_scsiHostPhyReset || (parityError && *parityError))
    {
        return 0;
    }
    else
    {
        if (count < fullcount)
        {
            logmsg("scsiHostRead: received ", (int)count, " bytes, expected ", (int)fullcount);
        }

        return count;
    }
}

// Release bus signals and expect the target to do the same.
// Cycles ACK in case target still holds BSY and REQ.
void scsiHostWaitBusFree()
{
    SCSI_RELEASE_OUTPUTS();

    sleep_us(2);

    // Wait for the target to release BSY signal.
    // If the target is expecting more data, transfer dummy bytes.
    // This happens for some reason with READ6 command on IBM H3171-S2.
    uint32_t start = millis();
    int extra_bytes = 0;
    while (SCSI_IN(BSY))
    {
        platform_poll();

        if (SCSI_IN(REQ))
        {
            // Target is expecting something more
            // Transfer dummy bytes
             SCSI_OUT(BSY, 1);
             sleep_us(1);

             while (SCSI_IN(REQ))
             {
                scsiHostReadOneByte(nullptr);
                extra_bytes++;
                sleep_us(1);
             }

             SCSI_OUT(BSY, 0);
             sleep_us(1);
        }

        if ((uint32_t)(millis() - start) > 10000)
        {
            logmsg("Target is holding BSY for unexpectedly long, running reset.");
            scsiHostPhyReset();
            break;
        }
    }

    if (extra_bytes > 0)
    {
        dbgmsg("---- Target requested ", extra_bytes, " extra bytes after command complete");
    }

    scsiHostPhyRelease();
}

// Release all bus signals
void scsiHostPhyRelease()
{
    scsiLogInitiatorPhaseChange(BUS_FREE);
    SCSI_RELEASE_OUTPUTS();
}

#endif
