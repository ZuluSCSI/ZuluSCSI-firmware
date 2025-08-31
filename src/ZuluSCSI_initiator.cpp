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


/*
 * Main program for initiator mode.
 */
#include "ZuluSCSI_config.h"
#include "ZuluSCSI_log.h"
#include "ZuluSCSI_log_trace.h"
#include "ZuluSCSI_initiator.h"
#include "ZuluSCSI_msc_initiator.h"
#include "ZuluSCSI_msc.h"
#include <ZuluSCSI_platform.h>
#include <minIni.h>
#include "SdFat.h"

#include "ui.h"

#include <scsi2sd.h>
extern "C" {
#include <scsi.h>
}

#ifndef PLATFORM_HAS_INITIATOR_MODE

void scsiInitiatorInit()
{
}

void scsiInitiatorMainLoop()
{
}

int scsiInitiatorRunCommand(const uint8_t *command, size_t cmdlen,
                            uint8_t *bufIn, size_t bufInLen,
                            const uint8_t *bufOut, size_t bufOutLen)
{
    return -1;
}

bool scsiInitiatorReadCapacity(int target_id, uint32_t *sectorcount, uint32_t *sectorsize)
{
    return false;
}

#else

// From ZuluSCSI.cpp
extern bool g_sdcard_present;

/*************************************
 * High level initiator mode logic   *
 *************************************/

static struct {
    // Bitmap of all drives that have been imaged
    uint32_t drives_imaged;

    // Configuration from .ini
    uint8_t initiator_id;
    uint8_t max_retry_count;
    bool use_read10; // Always use read10 commands

    // Is imaging a drive in progress, or are we scanning?
    bool imaging;

    // Information about currently selected drive
    int target_id;
    uint32_t sectorsize;
    uint32_t sectorcount;
    uint32_t sectorcount_all;
    uint32_t sectors_done;
    uint32_t max_sector_per_transfer;
    uint32_t bad_sector_count;
    uint8_t ansi_version;
    uint8_t device_type;

    // Retry information for sector reads.
    // If a large read fails, retry is done sector-by-sector.
    int retrycount;
    uint32_t failposition;
    bool eject_when_done;
    bool removable;

    uint32_t removable_count[8];

    // Negotiated bus width for targets
    int targetBusWidth[S2S_MAX_TARGETS];

    FsFile target_file;
} g_initiator_state;

extern SdFs SD;
static bool g_pause = false;

// Initialization of initiator mode
void scsiInitiatorInit()
{
    scsiHostPhyReset();

    g_initiator_state.initiator_id = ini_getl("SCSI", "InitiatorID", 7, CONFIGFILE);
    if (g_initiator_state.initiator_id > 7)
    {
        logmsg("InitiatorID set to illegal value in, ", CONFIGFILE, ", defaulting to 7");
        g_initiator_state.initiator_id = 7;
    }
    else
    {
        logmsg("InitiatorID set to ID ", g_initiator_state.initiator_id);
    }
    g_initiator_state.max_retry_count = ini_getl("SCSI", "InitiatorMaxRetry", 5, CONFIGFILE);
    g_initiator_state.use_read10 = ini_getbool("SCSI", "InitiatorUseRead10", false, CONFIGFILE);

    // treat initiator id as already imaged drive so it gets skipped
    g_initiator_state.drives_imaged = 1 << g_initiator_state.initiator_id;

    g_initiator_state.imaging = false;
    g_initiator_state.target_id = -1;
    g_initiator_state.sectorsize = 0;
    g_initiator_state.sectorcount = 0;
    g_initiator_state.sectors_done = 0;
    g_initiator_state.retrycount = 0;
    g_initiator_state.failposition = 0;
    g_initiator_state.max_sector_per_transfer = 512;
    g_initiator_state.ansi_version = 0;
    g_initiator_state.bad_sector_count = 0;
    g_initiator_state.device_type = SCSI_DEVICE_TYPE_DIRECT_ACCESS;
    g_initiator_state.removable = false;
    g_initiator_state.eject_when_done = false;
    memset(g_initiator_state.removable_count, 0, sizeof(g_initiator_state.removable_count));
    platform_led_breath(true, 0);

}

int scsiInitiatorGetOwnID()
{
    return g_initiator_state.initiator_id;
}

// Update progress bar LED during transfers
static void scsiInitiatorUpdateLed()
{
    // Update status indicator, the led blinks every 5 seconds and is on the longer the more data has been transferred
    const int period = 256;
    int phase = (millis() % period);
    int duty = (int64_t)g_initiator_state.sectors_done * period / g_initiator_state.sectorcount;

    // Minimum and maximum time to verify that the blink is visible
    if (duty < 50) duty = 50;
    if (duty > period - 50) duty = period - 50;

    if (phase <= duty)
    {
        LED_ON();
    }
    else
    {
        LED_OFF();
    }
}

uint8_t ejectButtonUpdate()
{
    // treat '1' to '0' transitions as reset actions
    static uint8_t previous = 0x00;
    uint8_t bitmask = platform_get_buttons() & EJECT_BTN_MASK;
    uint8_t ejectors = (previous ^ bitmask) & previous;
    previous = bitmask;
    return ejectors;
}

static void pollPauseOnEjectButton()
{
    if (ejectButtonUpdate() == 1)
    {
        logmsg("Eject button detected while in initiator mode, resetting state...");
        g_pause = true;
    }
}

void delay_with_poll(uint32_t ms)
{
    uint32_t start = millis();
    while ((uint32_t)(millis() - start) < ms)
    {
        pollPauseOnEjectButton();
        platform_poll();
        delay(1);
    }
}

static int scsiTypeToIniType(int scsi_type, bool removable)
{
    int ini_type = -1;
    switch (scsi_type)
    {
        case SCSI_DEVICE_TYPE_DIRECT_ACCESS:
            ini_type = removable ? S2S_CFG_REMOVABLE : S2S_CFG_FIXED;
            break;
        case 1:
            ini_type = -1; // S2S_CFG_SEQUENTIAL
            break;
        case SCSI_DEVICE_TYPE_CD:
            ini_type = S2S_CFG_OPTICAL;
            break;
        case SCSI_DEVICE_TYPE_MO:
            ini_type = S2S_CFG_MO;
            break;
        default:
            ini_type = -1;
            break;
    }
    return ini_type;
}

// High level logic of the initiator mode
void scsiInitiatorMainLoop()
{
    if (g_scsiHostPhyReset)
    {
        logmsg("Executing BUS RESET after aborted command");
        scsiHostPhyReset();
    }

#ifdef PLATFORM_MASS_STORAGE
    if (g_msc_initiator)
    {
        poll_msc_initiator();
        platform_run_msc();
        return;
    }
    else
    {
        if (!g_sdcard_present || ini_getbool("SCSI", "InitiatorMSC", false, CONFIGFILE))
        {
            // This delay allows the USB serial console to connect immediately to the host
            // It also decreases the delay in callback processing of MSC commands
            int32_t msc_init_delay = ini_getl("SCSI", "InitiatorMSCInitDelay", MSC_INIT_DELAY, CONFIGFILE);
            if (msc_init_delay != MSC_INIT_DELAY)
                logmsg("Initiator init delay set in ", CONFIGFILE ," to ", (int)msc_init_delay, " milliseconds");
            delay(msc_init_delay);

            logmsg("Entering USB MSC initiator mode");
            platform_enter_msc();
            setup_msc_initiator();
            return;
        }
    }
#endif

    if (!g_sdcard_present)
    {
        // Wait for SD card
        return;
    }

    pollPauseOnEjectButton();
    if (g_pause)
    {
        g_initiator_state.target_file.close();
        scsiInitiatorInit();
        logmsg("Initiator reset, pausing. Press eject to start initiator...");
        LED_OFF();
        platform_set_blink_status(false);
        platform_led_breath(true, PLATFORM_LED_PWM_BREATH_PERIOD_MS / 4);
        while(ejectButtonUpdate() != 1)
        {
            platform_poll();
            platform_reset_watchdog();
        }

        platform_led_breath(true, 0);
        g_pause = false;
        scsiHostPhyReset();
    }

    if (!g_initiator_state.imaging)
    {
        // Scan for SCSI drives one at a time
        g_initiator_state.target_id = (g_initiator_state.target_id + 1) % 8;
        g_initiator_state.sectorsize = 0;
        g_initiator_state.sectorcount = 0;
        g_initiator_state.sectors_done = 0;
        g_initiator_state.retrycount = 0;
        g_initiator_state.failposition = 0;
        g_initiator_state.max_sector_per_transfer = 512;
        g_initiator_state.ansi_version = 0;
        g_initiator_state.bad_sector_count = 0;
        g_initiator_state.device_type = SCSI_DEVICE_TYPE_DIRECT_ACCESS;
        g_initiator_state.removable = false;
        g_initiator_state.eject_when_done = false;
        g_initiator_state.use_read10 = false;

        UIInitiatorScanning(g_initiator_state.target_id);
        
        if (!(g_initiator_state.drives_imaged & (1 << g_initiator_state.target_id)))
        {
            delay_with_poll(1000);

            uint8_t inquiry_data[36] = {0};

            LED_ON();

            bool startstopok =
                scsiTestUnitReady(g_initiator_state.target_id) &&
                scsiStartStopUnit(g_initiator_state.target_id, true);

#if defined(PLATFORM_MAX_BUS_WIDTH) && PLATFORM_MAX_BUS_WIDTH > 0
            if (startstopok)
            {
                // Negotiate bus width
                // This is done before other commands just in case the target
                // happens to be in 16-bit mode. Only commands that have no
                // data phase can be used before this.
                int configBusWidth = ini_getl("SCSI", "InitiatorBusWidth", PLATFORM_MAX_BUS_WIDTH, CONFIGFILE);
                bool busWidthSet = scsiInitiatorSetBusWidth(g_initiator_state.target_id, configBusWidth);
                if (!busWidthSet && ini_haskey("SCSI", "InitiatorBusWidth", CONFIGFILE))
                {
                    logmsg("-- Failed to negotiate ", 8 << configBusWidth, " bit bus width that is forced in .ini file");
                    logmsg("-- Refusing to connect at lower bus width");
                    return;
                }
            }
#endif

            bool readcapok = startstopok &&
                scsiInitiatorReadCapacity(g_initiator_state.target_id,
                                          &g_initiator_state.sectorcount,
                                          &g_initiator_state.sectorsize);

            bool inquiryok = startstopok &&
                scsiInquiry(g_initiator_state.target_id, inquiry_data);

            LED_OFF();

            uint64_t total_bytes = 0;
            if (readcapok)
            {
                logmsg("SCSI ID ", g_initiator_state.target_id,
                    " capacity ", (int)g_initiator_state.sectorcount,
                    " sectors x ", (int)g_initiator_state.sectorsize, " bytes");

                UIInitiatorReadCapOk(g_initiator_state.target_id, g_initiator_state.device_type, g_initiator_state.sectorcount, g_initiator_state.sectorsize);
                
                g_initiator_state.sectorcount_all = g_initiator_state.sectorcount;

                total_bytes = (uint64_t)g_initiator_state.sectorcount * g_initiator_state.sectorsize;
                logmsg("Drive total size is ", (int)(total_bytes / (1024 * 1024)), " MiB");
                if (total_bytes >= 0xFFFFFFFF && SD.fatType() != FAT_TYPE_EXFAT)
                {
                    // GT TODO
                    // Note: the FAT32 limit is 4 GiB - 1 byte
                    logmsg("Target SCSI ID ", g_initiator_state.target_id, " image size is equal or larger than 4 GiB.");
                    logmsg("This is larger than the max filesize supported by SD card's filesystem");
                    logmsg("Please reformat the SD card with exFAT format to image this target");
                    g_initiator_state.drives_imaged |= 1 << g_initiator_state.target_id;
                    return;
                }
            }
            else if (startstopok)
            {
                // GT TODO
                logmsg("SCSI ID ", g_initiator_state.target_id, " responds but ReadCapacity command failed");
                logmsg("Possibly SCSI-1 drive? Attempting to read up to 1 GB.");
                g_initiator_state.sectorsize = 512;
                g_initiator_state.sectorcount = g_initiator_state.sectorcount_all = 2097152;
                g_initiator_state.max_sector_per_transfer = 128;
            }
            else
            {
#ifndef ZULUSCSI_NETWORK
                dbgmsg("Failed to connect to SCSI ID ", g_initiator_state.target_id);
#endif
                g_initiator_state.sectorsize = 0;
                g_initiator_state.sectorcount = g_initiator_state.sectorcount_all = 0;
            }

            char filename_base[12];
            strncpy(filename_base, "HD00_imaged", sizeof(filename_base));
            const char *filename_extension = ".hda";

            if (inquiryok)
            {
                char vendor[9], product[17], revision[5];
                g_initiator_state.device_type=inquiry_data[0] & 0x1f;
                g_initiator_state.ansi_version = inquiry_data[2] & 0x7;
                g_initiator_state.removable = !!(inquiry_data[1] & 0x80);
                g_initiator_state.eject_when_done = g_initiator_state.removable;
                memcpy(vendor, &inquiry_data[8], 8);
                vendor[8]=0;
                memcpy(product, &inquiry_data[16], 16);
                product[16]=0;
                memcpy(revision, &inquiry_data[32], 4);
                revision[4]=0;

                g_initiator_state.use_read10 = scsiInitiatorTestSupportsRead10(g_initiator_state.target_id, g_initiator_state.sectorsize);
                if(!g_initiator_state.use_read10)
                {
                    // READ6 command can transfer up to 256 sectors
                    g_initiator_state.max_sector_per_transfer = 256;
                }

                int ini_type = scsiTypeToIniType(g_initiator_state.device_type, g_initiator_state.removable);
                logmsg("SCSI Version ", (int) g_initiator_state.ansi_version);
                logmsg("[SCSI", g_initiator_state.target_id,"]");
                logmsg("  Vendor = \"", vendor,"\"");
                logmsg("  Product = \"", product,"\"");
                logmsg("  Version = \"", revision,"\"");
                if (ini_type == -1)
                    logmsg("Type = Not Supported, trying direct access");
                else
                    logmsg("  Type = ", ini_type);

                // GT TODO - info about the drive

                if (g_initiator_state.device_type == SCSI_DEVICE_TYPE_CD)
                {
                    strncpy(filename_base, "CD00_imaged", sizeof(filename_base));
                    filename_extension = ".iso";
                }
                else if (g_initiator_state.device_type == SCSI_DEVICE_TYPE_MO)
                {
                    strncpy(filename_base, "MO00_imaged", sizeof(filename_base));
                    filename_extension = ".img";
                }
                else if (g_initiator_state.device_type != SCSI_DEVICE_TYPE_DIRECT_ACCESS)
                {
                    logmsg("Unhandled scsi device type: ", g_initiator_state.device_type, ". Handling it as Direct Access Device.");
                    g_initiator_state.device_type = SCSI_DEVICE_TYPE_DIRECT_ACCESS;
                }

                if (g_initiator_state.device_type == SCSI_DEVICE_TYPE_DIRECT_ACCESS && g_initiator_state.removable)
                {
                    strncpy(filename_base, "RM00_imaged", sizeof(filename_base));
                    filename_extension = ".img";
                }
            }

            if (g_initiator_state.eject_when_done && g_initiator_state.removable_count[g_initiator_state.target_id] == 0)
            {
                g_initiator_state.removable_count[g_initiator_state.target_id] = 1;
            }

            if (g_initiator_state.sectorcount > 0)
            {
                char filename[32] = {0};
                filename_base[2] += g_initiator_state.target_id;
                if (g_initiator_state.eject_when_done)
                {
                    auto removable_count = g_initiator_state.removable_count[g_initiator_state.target_id];
                    snprintf(filename, sizeof(filename), "%s(%lu)%s",filename_base, removable_count, filename_extension);
                }
                else
                {
                    snprintf(filename, sizeof(filename), "%s%s", filename_base, filename_extension);
                }
                static int handling = -1;
                if (handling == -1)
                {
                    handling = ini_getl("SCSI", "InitiatorImageHandling", 0, CONFIGFILE);
                }
                // Stop if a file already exists
                if (handling == 0)
                {
                    if (SD.exists(filename))
                    {
                        // GT TODO
                        logmsg("File, ", filename, ", already exists, InitiatorImageHandling set to stop if file exists.");
                        g_initiator_state.drives_imaged |= (1 << g_initiator_state.target_id);
                        return;
                    }
                }
                // Create a new copy to the file 002-999
                else if (handling == 1)
                {
                    for (uint32_t i = 1; i <= 1000; i++)
                    {
                        if (i == 1)
                        {
                            if (SD.exists(filename))
                                continue;
                            break;
                        }
                        else if(i >= 1000)
                        {
                            // GT TODO
                            logmsg("Max images created from SCSI ID ", g_initiator_state.target_id, ", skipping image creation");
                            g_initiator_state.drives_imaged |= (1 << g_initiator_state.target_id);
                            return;
                        }
                        char filename_copy[6] = {0};
                        if (g_initiator_state.eject_when_done)
                        {
                            auto removable_count = g_initiator_state.removable_count[g_initiator_state.target_id];
                            snprintf(filename, sizeof(filename), "%s(%lu)-%03lu%s", filename_base, removable_count, i, filename_extension);
                        }
                        else
                        {
                            snprintf(filename, sizeof(filename), "%s-%03lu%s", filename_base, i, filename_extension);
                        }
                        snprintf(filename_copy, sizeof(filename_copy), "-%03lu", i);
                        if (SD.exists(filename))
                            continue;
                        break;
                    }

                }
                // overwrite file if it exists
                else if (handling == 2)
                {
                    if (SD.exists(filename))
                    {
                        // GT TODO
                        logmsg("File, ",filename, " already exists, InitiatorImageHandling set to overwrite file");
                        SD.remove(filename);
                    }
                }
                // InitiatorImageHandling invalid setting
                else
                {
                    static bool invalid_logged_once = false;
                    if (!invalid_logged_once)
                    {
                        logmsg("InitiatorImageHandling is set to, ", handling, ", which is invalid");
                        invalid_logged_once = true;
                    }
                    return;
                }

                uint64_t sd_card_free_bytes = (uint64_t)SD.vol()->freeClusterCount() * SD.vol()->bytesPerCluster();
                if (sd_card_free_bytes < total_bytes)
                {
                    // GT TODO
                    logmsg("SD Card only has ", (int)(sd_card_free_bytes / (1024 * 1024)),
                           " MiB - not enough free space to image SCSI ID ", g_initiator_state.target_id);
                    g_initiator_state.drives_imaged |= 1 << g_initiator_state.target_id;
                    return;
                }

                g_initiator_state.target_file = SD.open(filename, O_WRONLY | O_CREAT | O_TRUNC);
                if (!g_initiator_state.target_file.isOpen())
                {
                    logmsg("Failed to open file for writing: ", filename);
                    return;
                }

                if (SD.fatType() == FAT_TYPE_EXFAT)
                {
                    // Only preallocate on exFAT, on FAT32 preallocating can result in false garbage data in the
                    // file if write is interrupted.
                    logmsg("Preallocating image file");
                    g_initiator_state.target_file.preAllocate((uint64_t)g_initiator_state.sectorcount * g_initiator_state.sectorsize);
                }

                UIInitiatorTargetFilename(g_initiator_state.target_id, filename);

                logmsg("Starting to copy drive data to ", filename);
                g_initiator_state.imaging = true;
            }
        }
    }
    else
    {
        // Copy sectors from SCSI drive to file
        if (g_initiator_state.sectors_done >= g_initiator_state.sectorcount)
        {
            scsiStartStopUnit(g_initiator_state.target_id, false);
            logmsg("Finished imaging drive with id ", g_initiator_state.target_id);
            LED_OFF();

            UIInitiatorImagingComplete(g_initiator_state.target_id);
            
            if (g_initiator_state.sectorcount != g_initiator_state.sectorcount_all)
            {

                // GT TODO
                logmsg("NOTE: Image size was limited to first 4 GiB due to SD card filesystem limit");
                logmsg("Please reformat the SD card with exFAT format to image this drive fully");
            }

            if(g_initiator_state.bad_sector_count != 0)
            {
                // GT TODO
                logmsg("NOTE: There were ",  (int) g_initiator_state.bad_sector_count, " bad sectors that could not be read off this drive.");
            }

            if (!g_initiator_state.eject_when_done)
            {
                // GT TODO
                logmsg("Marking SCSI ID, ", g_initiator_state.target_id, ", as imaged, wont ask it again.");
                g_initiator_state.drives_imaged |= (1 << g_initiator_state.target_id);
            }

            g_initiator_state.imaging = false;
            g_initiator_state.target_file.close();
            return;
        }

        scsiInitiatorUpdateLed();

        // How many sectors to read in one batch?
        int numtoread = g_initiator_state.sectorcount - g_initiator_state.sectors_done;
        if (numtoread > g_initiator_state.max_sector_per_transfer)
            numtoread = g_initiator_state.max_sector_per_transfer;

        // Retry sector-by-sector after failure
        if (g_initiator_state.sectors_done < g_initiator_state.failposition)
            numtoread = 1;

        uint32_t time_start = millis();
        bool status = scsiInitiatorReadDataToFile(g_initiator_state.target_id,
            g_initiator_state.sectors_done, numtoread, g_initiator_state.sectorsize,
            g_initiator_state.target_file);

        if (!status)
        {
            logmsg("Failed to transfer ", numtoread, " sectors starting at ", (int)g_initiator_state.sectors_done);

            UIInitiatorFailedToTransfer(g_initiator_state.target_id);          

            if (g_initiator_state.retrycount < g_initiator_state.max_retry_count)
            {
                logmsg("Retrying.. ", g_initiator_state.retrycount + 1, "/", (int) g_initiator_state.max_retry_count);
                delay_with_poll(200);
                // This reset causes some drives to hang and seems to have no effect if left off.
                // scsiHostPhyReset();
                delay_with_poll(200);

                g_initiator_state.retrycount++;
                g_initiator_state.target_file.seek((uint64_t)g_initiator_state.sectors_done * g_initiator_state.sectorsize);

                if (g_initiator_state.retrycount > 1 && numtoread > 1)
                {
                    // GT TODO
                    logmsg("Multiple failures, retrying sector-by-sector");
                    g_initiator_state.failposition = g_initiator_state.sectors_done + numtoread;
                }

                UIInitiatorRetry(g_initiator_state.target_id);
            }
            else
            {
                logmsg("Retry limit exceeded, skipping one sector");
                g_initiator_state.retrycount = 0;
                g_initiator_state.sectors_done++;
                g_initiator_state.bad_sector_count++;
                g_initiator_state.target_file.seek((uint64_t)g_initiator_state.sectors_done * g_initiator_state.sectorsize);

                UIInitiatorSkippedSector(g_initiator_state.target_id);
            }
        }
        else
        {
            g_initiator_state.retrycount = 0;
            g_initiator_state.sectors_done += numtoread;
            g_initiator_state.target_file.flush();

            int speed_kbps = numtoread * g_initiator_state.sectorsize / (millis() - time_start);
            logmsg("SCSI read succeeded, sectors done: ",
                  (int)g_initiator_state.sectors_done, " / ", (int)g_initiator_state.sectorcount,
                  " speed ", speed_kbps, " kB/s - ", 
                  (int)(100 * (int64_t)g_initiator_state.sectors_done / g_initiator_state.sectorcount), "%");

            UIInitiatorProgress(g_initiator_state.target_id, millis() - time_start, g_initiator_state.sectors_done, numtoread);
        }
    }
}

/*************************************
 * Low level command implementations *
 *************************************/

int scsiInitiatorRunCommand(int target_id,
                            const uint8_t *command, size_t cmdLen,
                            uint8_t *bufIn, size_t bufInLen,
                            const uint8_t *bufOut, size_t bufOutLen,
                            bool returnDataPhase, uint32_t timeout)
{

    if (!scsiHostPhySelect(target_id, g_initiator_state.initiator_id))
    {
#ifndef ZULUSCSI_NETWORK
        dbgmsg("------ Target ", target_id, " did not respond");
#endif
        scsiHostPhyRelease();
        return -1;
    }

    SCSI_PHASE phase;
    int status = -1;
    uint32_t start = millis();
    while ((phase = (SCSI_PHASE)scsiHostPhyGetPhase()) != BUS_FREE)
    {
        // If explicit timeout is specified, prevent watchdog from triggering too early.
        if ((uint32_t)(millis() - start) < timeout)
        {
            platform_reset_watchdog();
        }

        platform_poll();

        if (phase == MESSAGE_IN)
        {
            uint8_t msg = 0;
            scsiHostRead(&msg, 1);

            if (msg == MSG_COMMAND_COMPLETE)
            {
                break;
            }
        }
        else if (phase == MESSAGE_OUT)
        {
            uint8_t identify_msg = 0x80;
            scsiHostWrite(&identify_msg, 1);
        }
        else if (phase == COMMAND)
        {
            scsiHostWrite(command, cmdLen);
        }
        else if (phase == DATA_IN)
        {
            if (returnDataPhase) return 0;
            if (bufInLen == 0)
            {
                logmsg("DATA_IN phase but no data to receive!");
                status = -3;
                break;
            }

            scsiHostSetBusWidth(g_initiator_state.targetBusWidth[target_id]);
            uint32_t readCount = scsiHostRead(bufIn, bufInLen);
            scsiHostSetBusWidth(0);
            if (readCount != bufInLen)
            {
                logmsg("scsiHostRead failed, tried to read ", (int)bufInLen, " bytes, got ", (int)readCount);
                status = -2;
                break;
            }
        }
        else if (phase == DATA_OUT)
        {
            if (returnDataPhase) return 0;
            if (bufOutLen == 0)
            {
                logmsg("DATA_OUT phase but no data to send!");
                status = -3;
                break;
            }

            scsiHostSetBusWidth(g_initiator_state.targetBusWidth[target_id]);
            uint32_t writeCount = scsiHostWrite(bufOut, bufOutLen);
            scsiHostSetBusWidth(0);
            if (writeCount != bufOutLen)
            {
                logmsg("scsiHostWrite failed, was writing ", bytearray(bufOut, bufOutLen), " return value ", (int)writeCount);
                status = -2;
                break;
            }
        }
        else if (phase == STATUS)
        {
            uint8_t tmp = -1;
            scsiHostRead(&tmp, 1);
            status = tmp;
#ifndef ZULUSCSI_NETWORK
            dbgmsg("------ STATUS: ", tmp);
#endif
        }
    }

    scsiHostWaitBusFree();

    return status;
}

int scsiInitiatorMessage(int target_id,
    const uint8_t *msgOut, size_t msgOutLen,
    uint8_t *msgIn, size_t msgInBufSize, size_t *msgInLen,
    uint32_t timeout)
{
    uint8_t command[6] = {0x00, 0, 0, 0, 0, 0};

    scsiHostPhySetATN(true);

    if (!scsiHostPhySelect(target_id, g_initiator_state.initiator_id))
    {
        dbgmsg("------ Target ", target_id, " did not respond");
        scsiHostPhyRelease();
        return -1;
    }

    size_t dummy;
    if (!msgInLen) msgInLen = &dummy;
    *msgInLen = 0;

    size_t msgOutSent = 0;

    SCSI_PHASE phase;
    int status = -1;
    uint32_t start = millis();
    while ((phase = (SCSI_PHASE)scsiHostPhyGetPhase()) != BUS_FREE)
    {
        // If explicit timeout is specified, prevent watchdog from triggering too early.
        if ((uint32_t)(millis() - start) < timeout)
        {
            platform_reset_watchdog();
        }

        platform_poll();

        if (phase == MESSAGE_IN)
        {
            uint8_t msg = 0;
            scsiHostRead(&msg, 1);

            if (*msgInLen < msgInBufSize)
            {
                msgIn[*msgInLen] = msg;
                *msgInLen += 1;
            }

            if (status != -1 && msg == MSG_COMMAND_COMPLETE)
            {
                break;
            }
        }
        else if (phase == MESSAGE_OUT)
        {
            if (msgOutSent < msgOutLen)
            {
                scsiHostWrite(&msgOut[msgOutSent++], 1);
                if (msgOutSent >= msgOutLen)
                {
                    // End of MESSAGE_OUT phase
                    // Note that target may switch to MESSAGE_IN earlier than this
                    scsiHostPhySetATN(false);
                }
            }
        }
        else if (phase == COMMAND)
        {
            scsiHostWrite(command, sizeof(command));
        }
        else if (phase == STATUS)
        {
            uint8_t tmp = -1;
            scsiHostRead(&tmp, 1);
            status = tmp;
            dbgmsg("------ STATUS: ", tmp);
        }
    }

    scsiHostWaitBusFree();

    return status;
}

bool scsiInitiatorTestSupportsRead10(int target_id, uint32_t sectorsize)
{
    if (ini_haskey("SCSI", "InitiatorUseRead10", CONFIGFILE))
    {
        return ini_getbool("SCSI", "InitiatorUseRead10", false, CONFIGFILE);
    }

    uint8_t command[10] = {0x28, 0x00, 0, 0, 0, 0, 0, 0, 1, 0}; // READ10, LBA 0, 1 sector
    int status = scsiInitiatorRunCommand(target_id, command, sizeof(command),
        scsiDev.data, sectorsize, NULL, 0);

    if (status == 0)
    {
        dbgmsg("Target supports READ10 command");
        return true;
    }
    else
    {
        dbgmsg("Target does not support READ10 command");
        return false;
    }
}

bool scsiInitiatorReadCapacity(int target_id, uint32_t *sectorcount, uint32_t *sectorsize)
{
    uint8_t command[10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t response[8] = {0};
    int status = scsiInitiatorRunCommand(target_id,
                                         command, sizeof(command),
                                         response, sizeof(response),
                                         NULL, 0);

    if (status == 0)
    {
        *sectorcount = ((uint32_t)response[0] << 24)
                    | ((uint32_t)response[1] << 16)
                    | ((uint32_t)response[2] <<  8)
                    | ((uint32_t)response[3] <<  0);

        *sectorcount += 1; // SCSI reports last sector address

        *sectorsize = ((uint32_t)response[4] << 24)
                    | ((uint32_t)response[5] << 16)
                    | ((uint32_t)response[6] <<  8)
                    | ((uint32_t)response[7] <<  0);

        return true;
    }
    else if (status == 2)
    {
        uint8_t sense_key;
        scsiRequestSense(target_id, &sense_key);
        scsiLogInitiatorCommandFailure("READ CAPACITY", target_id, status, sense_key);
        return false;
    }
    else
    {
        *sectorcount = *sectorsize = 0;
        return false;
    }
}

// Execute REQUEST SENSE command to get more information about error status
bool scsiRequestSense(int target_id, uint8_t *sense_key, uint8_t *sense_asc, uint8_t *sense_ascq)
{
    uint8_t command[6] = {0x03, 0, 0, 0, 18, 0};
    uint8_t response[18] = {0};

    int status = scsiInitiatorRunCommand(target_id,
                                         command, sizeof(command),
                                         response, sizeof(response),
                                         NULL, 0);

    dbgmsg("RequestSense response: ", bytearray(response, 18),
        " sense_key ", (int)(response[2] & 0xF),
        " asc ", response[12], " ascq ", response[13]);

    if (sense_key) *sense_key = response[2] & 0xF;
    if (sense_asc) *sense_asc = response[12];
    if (sense_ascq) *sense_ascq = response[13];

    return status == 0;
}

// Execute UNIT START STOP command to load/unload media
bool scsiStartStopUnit(int target_id, bool start)
{
    uint8_t command[6] = {0x1B, 0x1, 0, 0, 0, 0};
    uint8_t response[4] = {0};

    if (start)
    {
        command[4] |= 1; // Start
        command[1] = 0;  // Immediate
    }
    else // stop
    {
        if(g_initiator_state.eject_when_done)
        {
            logmsg("Ejecting media on SCSI ID: ", target_id);
            g_initiator_state.removable_count[g_initiator_state.target_id]++;
            command[4] = 0b00000010; // eject(6), stop(7).
        }
    }

    uint32_t timeout = 60000; // Some drives can take long to initialize
    int status = scsiInitiatorRunCommand(target_id,
                                         command, sizeof(command),
                                         response, sizeof(response),
                                         NULL, 0, false, timeout);

    if (status == 2)
    {
        uint8_t sense_key;
        scsiRequestSense(target_id, &sense_key);
        scsiLogInitiatorCommandFailure("START STOP UNIT", target_id, status, sense_key);

        if (sense_key == NOT_READY)
        {
            dbgmsg("--- Device reports NOT_READY, running STOP to attempt restart");
            // Some devices will only leave NOT_READY state after they have been
            // commanded to stop state first.
            delay(1000);
            uint8_t cmd_stop[6] = {0x1B, 0x1, 0, 0, 0, 0};
            scsiInitiatorRunCommand(target_id,
                                    cmd_stop, sizeof(cmd_stop),
                                    response, sizeof(response),
                                    NULL, 0);
        }
    }

    return status == 0;
}

// Execute INQUIRY command
bool scsiInquiry(int target_id, uint8_t inquiry_data[36])
{
    uint8_t command[6] = {0x12, 0, 0, 0, 36, 0};
    int status = scsiInitiatorRunCommand(target_id,
                                         command, sizeof(command),
                                         inquiry_data, 36,
                                         NULL, 0);
    return status == 0;
}

// Execute TEST UNIT READY command and handle unit attention state
bool scsiTestUnitReady(int target_id)
{
    for (int retries = 0; retries < 2; retries++)
    {
        uint8_t command[6] = {0x00, 0, 0, 0, 0, 0};
        int status = scsiInitiatorRunCommand(target_id,
                                            command, sizeof(command),
                                            NULL, 0,
                                            NULL, 0);

        if (status == 0)
        {
            return true;
        }
        else if (status == -1)
        {
            // No response to select
            return false;
        }
        else if (status == 2)
        {
            uint8_t sense_key;
            scsiRequestSense(target_id, &sense_key);

            if (sense_key == UNIT_ATTENTION)
            {
                uint8_t inquiry[36];
                dbgmsg("Target ", target_id, " reports UNIT_ATTENTION, running INQUIRY");
                scsiInquiry(target_id, inquiry);
            }
            else if (sense_key == NOT_READY)
            {
                dbgmsg("Target ", target_id, " reports NOT_READY, running STARTSTOPUNIT");
                scsiStartStopUnit(target_id, true);
            }
        }
        else
        {
            dbgmsg("Target ", target_id, " TEST UNIT READY response: ", status);
        }
    }

    return false;
}

bool scsiInitiatorResetBusConfig(int target_id)
{
    uint8_t msgOut[] = {0x80, // Identify
        0x01, 0x03, 0x01, 0x00, 0x00, // Disable synchronous mode
        0x01, 0x02, 0x03, 0x00  // 8-bit mode
    };

    g_initiator_state.targetBusWidth[target_id] = 0;

    int status = scsiInitiatorMessage(target_id, msgOut, sizeof(msgOut), nullptr, 0, nullptr);
    return status == 0;
}

#if !defined(PLATFORM_MAX_BUS_WIDTH) || PLATFORM_MAX_BUS_WIDTH == 0
bool scsiInitiatorSetBusWidth(int target_id, int busWidth)
{
    return false;
}
#else
bool scsiInitiatorSetBusWidth(int target_id, int busWidth)
{
    uint8_t msgOut[] = {0x80, // Identify
        0x01, 0x02, 0x03, (uint8_t)busWidth  // Bus width
    };

    uint8_t msgIn[16] = {0};
    size_t msgInLen = 0;

    dbgmsg("---- Negotiating bus width = ", (uint8_t)busWidth);
    int status = scsiInitiatorMessage(target_id, msgOut, sizeof(msgOut), msgIn, sizeof(msgIn), &msgInLen);
    if (status != 0)
    {
        scsiInitiatorResetBusConfig(target_id);
        return false;
    }

    // Parse response message
    int agreedMode = -1;
    size_t parsed = 0;
    while (parsed < msgInLen)
    {
        uint8_t msgByte = msgIn[parsed++];
        if (msgByte == 0x01)
        {
            // Extended message
            uint8_t extLen = msgIn[parsed++];
            uint8_t *extMsg = &msgIn[parsed];
            parsed += extLen;

            if (extMsg[0] == 0x03)
            {
                dbgmsg("-- Target bus width response: ", extMsg[1]);
                agreedMode = extMsg[1];
            }
        }
        else if ((msgByte & 0xF0) == 0x20)
        {
            // Two-byte message, ignore
            parsed++;
        }
    }

    if (agreedMode < 0)
    {
        logmsg("-- Target did not respond to bus width negotiation, reverting to 8-bit");
        scsiInitiatorResetBusConfig(target_id);
        return false;
    }
    else if (agreedMode == busWidth)
    {
        dbgmsg("-- Negotiated bus width ", 8 << busWidth, " bits, testing with Inquiry command");
        g_initiator_state.targetBusWidth[target_id] = busWidth;
        uint8_t inquiryData[36];
        if (!scsiInquiry(target_id, inquiryData))
        {
            logmsg("-- Bus width test failed, reverting to 8-bit");
            scsiInitiatorResetBusConfig(target_id);
            return false;
        }
        else
        {
            logmsg("-- Successfully negotiated ", 8 << busWidth, " bit bus mode");
            return true;
        }
    }
    else
    {
        logmsg("-- Target refused wide bus request, reverting to 8-bit");
        scsiInitiatorResetBusConfig(target_id);
        return false;
    }
}
#endif

// This uses callbacks to run SD and SCSI transfers in parallel
static struct {
    uint32_t bytes_sd; // Number of bytes that have been transferred on SD card side
    uint32_t bytes_sd_scheduled; // Number of bytes scheduled for transfer on SD card side
    uint32_t bytes_scsi; // Number of bytes that have been scheduled for transfer on SCSI side
    uint32_t bytes_scsi_done; // Number of bytes that have been transferred on SCSI side

    uint32_t bytes_per_sector;
    bool all_ok;
} g_initiator_transfer;

static void initiatorReadSDCallback(uint32_t bytes_complete)
{
    if (g_initiator_transfer.bytes_scsi_done < g_initiator_transfer.bytes_scsi)
    {
        // How many bytes remaining in the transfer?
        uint32_t remain = g_initiator_transfer.bytes_scsi - g_initiator_transfer.bytes_scsi_done;
        uint32_t len = remain;

        // Limit maximum amount of data transferred at one go, to give enough callbacks to SD driver.
        // Select the limit based on total bytes in the transfer.
        // Transfer size is reduced towards the end of transfer to reduce the dead time between
        // end of SCSI transfer and the SD write completing.
        uint32_t limit = g_initiator_transfer.bytes_scsi / 8;
        uint32_t bytesPerSector = g_initiator_transfer.bytes_per_sector;
        if (limit < PLATFORM_OPTIMAL_MIN_SD_WRITE_SIZE) limit = PLATFORM_OPTIMAL_MIN_SD_WRITE_SIZE;
        if (limit > PLATFORM_OPTIMAL_MAX_SD_WRITE_SIZE) limit = PLATFORM_OPTIMAL_MAX_SD_WRITE_SIZE;
        if (limit > len) limit = PLATFORM_OPTIMAL_LAST_SD_WRITE_SIZE;
        if (limit < bytesPerSector) limit = bytesPerSector;

        if (len > limit)
        {
            len = limit;
        }

        // Split read so that it doesn't wrap around buffer edge
        uint32_t bufsize = sizeof(scsiDev.data);
        uint32_t start = (g_initiator_transfer.bytes_scsi_done % bufsize);
        if (start + len > bufsize)
            len = bufsize - start;

        // Don't overwrite data that has not yet been written to SD card
        uint32_t sd_ready_cnt = g_initiator_transfer.bytes_sd + bytes_complete;
        if (g_initiator_transfer.bytes_scsi_done + len > sd_ready_cnt + bufsize)
            len = sd_ready_cnt + bufsize - g_initiator_transfer.bytes_scsi_done;

        if (sd_ready_cnt == g_initiator_transfer.bytes_sd_scheduled &&
            g_initiator_transfer.bytes_sd_scheduled + bytesPerSector <= g_initiator_transfer.bytes_scsi_done)
        {
            // Current SD transfer is complete, it is better we return now and offer a chance for the next
            // transfer to begin.
            return;
        }

        // Keep transfers a multiple of sector size.
        if (remain >= bytesPerSector && len % bytesPerSector != 0)
        {
            len -= len % bytesPerSector;
        }

        if (len == 0)
            return;

        // dbgmsg("SCSI read ", (int)start, " + ", (int)len, ", sd ready cnt ", (int)sd_ready_cnt, " ", (int)bytes_complete, ", scsi done ", (int)g_initiator_transfer.bytes_scsi_done);
        scsiHostSetBusWidth(g_initiator_state.targetBusWidth[g_initiator_state.target_id]);
        uint32_t rxcount = scsiHostRead(&scsiDev.data[start], len);
        scsiHostSetBusWidth(0);
        if (rxcount != len)
        {
            logmsg("Read failed at byte ", (int)g_initiator_transfer.bytes_scsi_done);
            g_initiator_transfer.all_ok = false;
        }
        g_initiator_transfer.bytes_scsi_done += len;
    }
}

static void scsiInitiatorWriteDataToSd(FsFile &file, bool use_callback)
{
    // Figure out longest continuous block in buffer
    uint32_t bufsize = sizeof(scsiDev.data);
    uint32_t start = g_initiator_transfer.bytes_sd % bufsize;
    uint32_t len = g_initiator_transfer.bytes_scsi_done - g_initiator_transfer.bytes_sd;
    if (start + len > bufsize) len = bufsize - start;

    // Try to do writes in multiple of 512 bytes
    // This allows better performance for SD card access.
    if (len >= 512) len &= ~511;

    // Start writing to SD card and simultaneously reading more from SCSI bus
    uint8_t *buf = &scsiDev.data[start];
    // dbgmsg("SD write ", (int)start, " + ", (int)len);

    if (use_callback)
    {
        platform_set_sd_callback(&initiatorReadSDCallback, buf);
    }

    g_initiator_transfer.bytes_sd_scheduled = g_initiator_transfer.bytes_sd + len;
    if (file.write(buf, len) != len)
    {
        logmsg("scsiInitiatorReadDataToFile: SD card write failed");
        g_initiator_transfer.all_ok = false;
    }
    platform_set_sd_callback(NULL, NULL);
    g_initiator_transfer.bytes_sd += len;
}

bool scsiInitiatorReadDataToFile(int target_id, uint32_t start_sector, uint32_t sectorcount, uint32_t sectorsize,
                                 FsFile &file)
{
    int status = -1;

    // Read6 command supports 21 bit LBA - max of 0x1FFFFF
    // ref: https://www.seagate.com/files/staticfiles/support/docs/manual/Interface%20manuals/100293068j.pdf pg 134
    bool fits_read6 = (start_sector < 0x1FFFFF && sectorcount <= 256);
    if (!g_initiator_state.use_read10 && fits_read6)
    {
        // Use READ6 command for compatibility with old SCSI1 drives
        // Note that even with SCSI1 drives we have no choice but to use READ10 if the drive
        // size is larger than 1 GB, as the sector number wouldn't fit in the command.
        uint8_t command[6] = {0x08,
            (uint8_t)(start_sector >> 16),
            (uint8_t)(start_sector >> 8),
            (uint8_t)start_sector,
            (uint8_t)sectorcount,
            0x00
        };

        // Start executing command, return in data phase
        status = scsiInitiatorRunCommand(target_id, command, sizeof(command), NULL, 0, NULL, 0, true);
    }
    else
    {
        // Use READ10 command for larger number of blocks
        uint8_t command[10] = {0x28, 0x00,
            (uint8_t)(start_sector >> 24), (uint8_t)(start_sector >> 16),
            (uint8_t)(start_sector >> 8), (uint8_t)start_sector,
            0x00,
            (uint8_t)(sectorcount >> 8), (uint8_t)(sectorcount),
            0x00
        };

        // Start executing command, return in data phase
        status = scsiInitiatorRunCommand(target_id, command, sizeof(command), NULL, 0, NULL, 0, true);
    }


    if (status != 0)
    {
        uint8_t sense_key;
        scsiRequestSense(target_id, &sense_key);

        scsiLogInitiatorCommandFailure("scsiInitiatorReadDataToFile command phase", target_id, status, sense_key);
        scsiHostPhyRelease();
        return false;
    }

    SCSI_PHASE phase;

    g_initiator_transfer.bytes_scsi = sectorcount * sectorsize;
    g_initiator_transfer.bytes_per_sector = sectorsize;
    g_initiator_transfer.bytes_sd = 0;
    g_initiator_transfer.bytes_sd_scheduled = 0;
    g_initiator_transfer.bytes_scsi_done = 0;
    g_initiator_transfer.all_ok = true;

    while (true)
    {
        platform_poll();

        phase = (SCSI_PHASE)scsiHostPhyGetPhase();
        if (phase != DATA_IN && phase != BUS_BUSY)
        {
            break;
        }

        // Read next block from SCSI bus if buffer empty
        if (g_initiator_transfer.bytes_sd == g_initiator_transfer.bytes_scsi_done)
        {
            initiatorReadSDCallback(0);
        }
        else
        {
            // Write data to SD card and simultaneously read more from SCSI
            scsiInitiatorUpdateLed();
            scsiInitiatorWriteDataToSd(file, true);
        }
    }

    // Write any remaining buffered data
    while (g_initiator_transfer.bytes_sd < g_initiator_transfer.bytes_scsi_done)
    {
        platform_poll();
        scsiInitiatorWriteDataToSd(file, false);
    }

    if (g_initiator_transfer.bytes_sd != g_initiator_transfer.bytes_scsi)
    {
        logmsg("SCSI read from sector ", (int)start_sector, " was incomplete: expected ",
             (int)g_initiator_transfer.bytes_scsi, " got ", (int)g_initiator_transfer.bytes_sd, " bytes");
        g_initiator_transfer.all_ok = false;
    }

    while ((phase = (SCSI_PHASE)scsiHostPhyGetPhase()) != BUS_FREE)
    {
        platform_poll();

        if (phase == MESSAGE_IN)
        {
            uint8_t msg = 0;
            scsiHostRead(&msg, 1);

            if (msg == MSG_COMMAND_COMPLETE)
            {
                break;
            }
        }
        else if (phase == MESSAGE_OUT)
        {
            uint8_t identify_msg = 0x80;
            scsiHostWrite(&identify_msg, 1);
        }
        else if (phase == STATUS)
        {
            uint8_t tmp = 0;
            scsiHostRead(&tmp, 1);
            status = tmp;
            dbgmsg("------ STATUS: ", tmp);
        }
    }

    scsiHostWaitBusFree();

    if (!g_initiator_transfer.all_ok)
    {
        dbgmsg("scsiInitiatorReadDataToFile: Incomplete transfer");
        return false;
    }
    else if (status == 2)
    {
        uint8_t sense_key;
        scsiRequestSense(target_id, &sense_key);

        if (sense_key == RECOVERED_ERROR)
        {
            dbgmsg("scsiInitiatorReadDataToFile: RECOVERED_ERROR at ", (int)start_sector);
            return true;
        }
        else if (sense_key == UNIT_ATTENTION)
        {
            dbgmsg("scsiInitiatorReadDataToFile: UNIT_ATTENTION");
            return true;
        }
        else
        {
            scsiLogInitiatorCommandFailure("scsiInitiatorReadDataToFile data phase", target_id, status, sense_key);
            return false;
        }
    }
    else
    {
        return status == 0;
    }
}


#endif
