#include "ZuluSCSI_platform.h"

#ifndef PLATFORM_HAS_SNIFFER
bool platform_init_sniffer() { return false; }
void platform_sniffer_poll() { }

#else

#include "ZuluSCSI.h"
#include "ZuluSCSI_log.h"
#include "minIni.h"
#include "rp2350_sniffer.h"
#include "SdFat.h"
#include <hardware/pio.h>

#ifdef ENABLE_AUDIO_OUTPUT_I2S
#include "audio_i2s.h"
#endif


#ifndef SNIFFER_MAX_BLOCKS_PER_POLL
#define SNIFFER_MAX_BLOCKS_PER_POLL 32
#endif

static struct {
    bool enabled;
    FsFile file;

    uint32_t last_status_message_time;

    uint32_t last_sync_time;
    uint32_t logpos;

    sniffer_block_t *sd_blocks[SNIFFER_BLOCKCOUNT];
    uint32_t sd_blocks_complete;
} g_zulusniffer;

extern "C" void sniffer_log(const char *msg)
{
    logmsg(msg);
}

bool platform_init_sniffer()
{
#ifdef ENABLE_AUDIO_OUTPUT_I2S
    logmsg("-- Disabling audio output to enable sniffer");
    audio_disable();
#endif

    pio_set_gpio_base(SNIFFER_PIO, 0);

    // Reset state
    g_zulusniffer.last_sync_time = 0;
    g_zulusniffer.logpos = 0;
    g_zulusniffer.enabled = false;
    g_zulusniffer.file.close();
    SD.remove(SNIFFERFILE);

    g_zulusniffer.file = SD.open(SNIFFERFILE, O_WRONLY | O_CREAT | O_TRUNC);
    if (!g_zulusniffer.file.isOpen())
    {
        logmsg("-- Failed to open ", SNIFFERFILE , " for writing");
        return false;
    }

    uint32_t pinmask = ini_getl("SCSI", "sniffer_trigpins", SNIFFER_DEFAULT_TRIGPINS, CONFIGFILE);
    if (rp2350_sniffer_start(pinmask))
    {
        logmsg("-- Storing SCSI bus traffic to " SNIFFERFILE);
        g_zulusniffer.enabled = true;
        return true;
    }
    else
    {
        logmsg("-- Failed to initialize SCSI bus sniffer");
        return false;
    }
}

// Process new data from DMA while SD card is busy writing
static void sniffer_sd_callback(uint32_t bytes_complete)
{
    uint32_t blocks_complete = bytes_complete / SNIFFER_BLOCKSIZE;
    while (blocks_complete > g_zulusniffer.sd_blocks_complete)
    {
        // Release blocks to sniffer DMA when SD card has finished writing them out
        rp2350_sniffer_release(g_zulusniffer.sd_blocks[g_zulusniffer.sd_blocks_complete++]);
    }
}

void platform_sniffer_poll()
{
    if (!g_zulusniffer.enabled) return;

    if (!g_sdcard_present || !g_zulusniffer.file.isOpen())
    {
        rp2350_sniffer_stop();
        g_zulusniffer.file.close();
        g_zulusniffer.enabled = false;
        return;
    }

    // Push any new log data
    if (log_get_buffer_len() > g_zulusniffer.logpos)
    {
        uint32_t available = 0;
        const char *log = log_get_buffer(&g_zulusniffer.logpos, &available);

        // Write at most 512 bytes per poll iteration
        uint32_t to_write = (available > 512) ? 512 : available;
        uint32_t wrote = rp2350_sniffer_write_aux_data(SNIFFER_DATAID_SYSLOG, log, to_write);

        // If all data was not written, retry it next time
        if (available > wrote)
        {
            g_zulusniffer.logpos -= available - wrote;
        }
    }

    // Log status periodically
    uint32_t time_now = millis();
    if ((uint32_t)(time_now - g_zulusniffer.last_status_message_time) > 1000)
    {
        g_zulusniffer.last_status_message_time = time_now;
        rp2350_sniffer_stats_t stats;
        rp2350_sniffer_get_status(&stats);

        logmsg("-- Bus sniffer status: total ", (int)((stats.total_bytes + 1023) / 1024), " kB, ",
                (int)stats.overruns, " buffer overruns");
    }

    // Force flushing of data to filesystem every 2 seconds
    bool force_sync = ((uint32_t)(time_now - g_zulusniffer.last_sync_time) > 2000);
    
    // Fetch first block
    sniffer_block_t *next_block = rp2350_sniffer_fetch(force_sync);

    int total_blocks = 0;
    while (next_block)
    {
        // Fetch multiple consecutive blocks if available
        int blockcount = 0;
        sniffer_block_t *expected_next;
        memset(g_zulusniffer.sd_blocks, 0, sizeof(g_zulusniffer.sd_blocks));
        for (int i = 0; i < SNIFFER_BLOCKCOUNT; i++)
        {
            g_zulusniffer.sd_blocks[i] = next_block;
            blockcount++;
            total_blocks++;
            expected_next = next_block + 1;
            next_block = rp2350_sniffer_fetch(false);

            if (next_block != expected_next)
            {
                break;
            }
        }

        // Write out the consecutive blocks
        uint8_t *readptr = (uint8_t*)g_zulusniffer.sd_blocks[0];
        g_zulusniffer.sd_blocks_complete = 0;
        size_t to_write = blockcount * SNIFFER_BLOCKSIZE;
        platform_set_sd_callback(sniffer_sd_callback, nullptr);
        size_t wrote = g_zulusniffer.file.write(readptr, to_write);
        platform_set_sd_callback(nullptr, nullptr);

        // Finish the write operation and release blocks to sniffer DMA
        sniffer_sd_callback(to_write);

        if (wrote != to_write)
        {
            rp2350_sniffer_stop();
            logmsg("Sniffer write failed");
            g_zulusniffer.file.close();
            g_zulusniffer.enabled = false;
            break;
        }
    }

    if (force_sync)
    {
        // Update file size in FAT table
        g_zulusniffer.file.flush();
        g_zulusniffer.file.sync();
        g_zulusniffer.last_sync_time = millis();
    }
}

#endif
