/** 
 * ZuluSCSI™ - Copyright (c) 2022 Rabbit Hole Computing™
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

// Accelerated SCSI subroutines for SCSI initiator/host side communication

#include "scsi_accel_host.h"
#include "ZuluSCSI_platform.h"
#include "ZuluSCSI_log.h"
#include <hardware/pio.h>
#include <hardware/dma.h>
#include <hardware/irq.h>
#include <hardware/structs/iobank0.h>
#include <hardware/sync.h>

#ifdef PLATFORM_HAS_INITIATOR_MODE
# include "scsi_accel_host_RP2MCU.pio.h"

#define SCSI_PIO pio0
#define SCSI_SM 0

static struct {
    // PIO configurations
    uint32_t pio_offset_async_read;
    pio_sm_config pio_cfg_async_read;
} g_scsi_host;

enum scsidma_state_t { SCSIHOST_IDLE = 0,
                       SCSIHOST_READ };
static volatile scsidma_state_t g_scsi_host_state;

static void scsi_accel_host_config_gpio()
{
    if (g_scsi_host_state == SCSIHOST_IDLE)
    {
        iobank0_hw->io[SCSI_IO_DB0].ctrl  = GPIO_FUNC_SIO;
        iobank0_hw->io[SCSI_IO_DB1].ctrl  = GPIO_FUNC_SIO;
        iobank0_hw->io[SCSI_IO_DB2].ctrl  = GPIO_FUNC_SIO;
        iobank0_hw->io[SCSI_IO_DB3].ctrl  = GPIO_FUNC_SIO;
        iobank0_hw->io[SCSI_IO_DB4].ctrl  = GPIO_FUNC_SIO;
        iobank0_hw->io[SCSI_IO_DB5].ctrl  = GPIO_FUNC_SIO;
        iobank0_hw->io[SCSI_IO_DB6].ctrl  = GPIO_FUNC_SIO;
        iobank0_hw->io[SCSI_IO_DB7].ctrl  = GPIO_FUNC_SIO;
        iobank0_hw->io[SCSI_IO_DBP].ctrl  = GPIO_FUNC_SIO;
        iobank0_hw->io[SCSI_IN_REQ].ctrl  = GPIO_FUNC_SIO;
        iobank0_hw->io[SCSI_OUT_ACK].ctrl = GPIO_FUNC_SIO;
    }
    else if (g_scsi_host_state == SCSIHOST_READ)
    {
        // Data bus and REQ as input, ACK pin as output
        pio_sm_set_pins(SCSI_PIO, SCSI_SM, SCSI_IO_DATA_MASK | 1 << SCSI_IN_REQ | 1 << SCSI_OUT_ACK);
        pio_sm_set_consecutive_pindirs(SCSI_PIO, SCSI_SM, SCSI_IO_DB0, 9, false);
        pio_sm_set_consecutive_pindirs(SCSI_PIO, SCSI_SM, SCSI_IN_REQ, 1, false);
        pio_sm_set_consecutive_pindirs(SCSI_PIO, SCSI_SM, SCSI_OUT_ACK, 1, true);

        iobank0_hw->io[SCSI_IO_DB0].ctrl  = GPIO_FUNC_SIO;
        iobank0_hw->io[SCSI_IO_DB1].ctrl  = GPIO_FUNC_SIO;
        iobank0_hw->io[SCSI_IO_DB2].ctrl  = GPIO_FUNC_SIO;
        iobank0_hw->io[SCSI_IO_DB3].ctrl  = GPIO_FUNC_SIO;
        iobank0_hw->io[SCSI_IO_DB4].ctrl  = GPIO_FUNC_SIO;
        iobank0_hw->io[SCSI_IO_DB5].ctrl  = GPIO_FUNC_SIO;
        iobank0_hw->io[SCSI_IO_DB6].ctrl  = GPIO_FUNC_SIO;
        iobank0_hw->io[SCSI_IO_DB7].ctrl  = GPIO_FUNC_SIO;
        iobank0_hw->io[SCSI_IO_DBP].ctrl  = GPIO_FUNC_SIO;
        iobank0_hw->io[SCSI_IN_REQ].ctrl  = GPIO_FUNC_SIO;
        iobank0_hw->io[SCSI_OUT_ACK].ctrl = GPIO_FUNC_PIO0;
    }
}

uint32_t scsi_accel_host_read(uint8_t *buf, uint32_t count, int *parityError, volatile int *resetFlag)
{
    // Currently this method just reads from the PIO RX fifo directly in software loop.
    // The SD card access is parallelized using DMA, so there is limited benefit from using DMA here.
    g_scsi_host_state = SCSIHOST_READ;

    int cd_start = SCSI_IN(CD);
    int msg_start = SCSI_IN(MSG);

    pio_sm_init(SCSI_PIO, SCSI_SM, g_scsi_host.pio_offset_async_read, &g_scsi_host.pio_cfg_async_read);
    scsi_accel_host_config_gpio();
    pio_sm_set_enabled(SCSI_PIO, SCSI_SM, true);

    // Set the number of bytes to read, must be divisible by 2.
    assert((count & 1) == 0);
    pio_sm_put(SCSI_PIO, SCSI_SM, count - 1);

    // Read results from PIO RX FIFO
    uint8_t *dst = buf;
    uint8_t *end = buf + count;
    uint32_t paritycheck = 0;
    uint32_t prev_rx_time = millis();
    while (dst < end)
    {
        uint32_t available = pio_sm_get_rx_fifo_level(SCSI_PIO, SCSI_SM);

        if (available == 0)
        {
            // No new data has been received by PIO, check if there is a need to abort

            bool abort = false;
            if (*resetFlag)
            {
                dbgmsg("scsi_accel_host_read: Aborting due to reset request");
                abort = true;
            }
            else if ((millis() - prev_rx_time) > 10000)
            {
                dbgmsg("scsi_accel_host_read: Aborting due to timeout");
                abort = true;
            }
            else
            {
                // Some drives such as ST-296N may have glitches on phase signals in between
                // byte transfers. This is allowed by SCSI spec, and officially we should only
                // check the phase signals when REQ is active. However the PIO logic currently
                // does not do this. Instead, when we detect a phase change, wait for 10 milliseconds
                // to see if it is real.
                int debounce = 100;
                while (debounce > 0 && (!SCSI_IN(IO) || SCSI_IN(CD) != cd_start || SCSI_IN(MSG) != msg_start))
                {
                    debounce--;
                    delayMicroseconds(100);
                }

                if (debounce == 0)
                {
                    dbgmsg("scsi_accel_host_read: aborting because target switched transfer phase (IO: ",
                        (int)SCSI_IN(IO), ", CD: ", (int)SCSI_IN(CD), ", MSG: ", (int)SCSI_IN(MSG), ")");
                    abort = true;
                }
            }

            if (abort)
            {
                count = dst - buf;
                break;
            }
        }

        while (available > 0)
        {
            available--;
            uint32_t word = pio_sm_get(SCSI_PIO, SCSI_SM);
            paritycheck ^= word;
            word = ~word;
            *dst++ = word & 0xFF;
            *dst++ = word >> 16;
        }
    }

    // Check parity errors in whole block
    // This doesn't detect if there is even number of parity errors in block.
    uint8_t byte0 = ~(paritycheck & 0xFF);
    uint8_t byte1 = ~(paritycheck >> 16);
    if (paritycheck != ((g_scsi_parity_lookup[byte1] << 16) | g_scsi_parity_lookup[byte0]))
    {
        logmsg("Parity error in scsi_accel_host_read(): ", paritycheck);
        *parityError = 1;
    }

    g_scsi_host_state = SCSIHOST_IDLE;
    SCSI_RELEASE_DATA_REQ();
    scsi_accel_host_config_gpio();
    pio_sm_set_enabled(SCSI_PIO, SCSI_SM, false);

    return count;
}


void scsi_accel_host_init()
{
    g_scsi_host_state = SCSIHOST_IDLE;
    scsi_accel_host_config_gpio();

    // Load PIO programs
    pio_clear_instruction_memory(SCSI_PIO);

    // Asynchronous / synchronous SCSI read
    g_scsi_host.pio_offset_async_read = pio_add_program(SCSI_PIO, &scsi_host_async_read_program);
    //    wait 0 gpio REQ             side 1  ; Wait for REQ low
    uint16_t instr = pio_encode_wait_gpio(false, SCSI_IN_REQ) | pio_encode_sideset(1, 1);
    SCSI_PIO->instr_mem[g_scsi_host.pio_offset_async_read + 2] = instr;
    instr =   pio_encode_wait_gpio(true, SCSI_IN_REQ) | pio_encode_sideset(1, 0);
    SCSI_PIO->instr_mem[g_scsi_host.pio_offset_async_read + 5] = instr;
    g_scsi_host.pio_cfg_async_read = scsi_host_async_read_program_get_default_config(g_scsi_host.pio_offset_async_read);
    sm_config_set_in_pins(&g_scsi_host.pio_cfg_async_read, SCSI_IO_DB0);
    sm_config_set_sideset_pins(&g_scsi_host.pio_cfg_async_read, SCSI_OUT_ACK);
    sm_config_set_out_shift(&g_scsi_host.pio_cfg_async_read, true, false, 32);
    sm_config_set_in_shift(&g_scsi_host.pio_cfg_async_read, true, true, 32);
}

#endif // PLATFORM_HAS_INITIATOR_MODE
