/* Initiator mode USB Mass Storage Class connection.
 * This file binds platform-specific MSC routines to the initiator mode
 * SCSI bus interface. The call structure is modeled after TinyUSB, but
 * should be usable with other USB libraries.
 *
 * ZuluSCSI™ - Copyright (c) 2023 Rabbit Hole Computing™
 *
 * This file is licensed under the GPL version 3 or any later version. 
 * It is derived from cdrom.c in SCSI2SD V6
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
 */


#include "ZuluSCSI_config.h"
#include "ZuluSCSI_log.h"
#include "ZuluSCSI_log_trace.h"
#include "ZuluSCSI_initiator.h"
#include <ZuluSCSI_platform.h>
#include <minIni.h>
#include "SdFat.h"

bool g_msc_initiator;

#ifndef PLATFORM_HAS_INITIATOR_MODE

bool setup_msc_initiator() { return false; }
void poll_msc_initiator() {}

void init_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4]) {}
uint8_t init_msc_get_maxlun_cb(void) { return 0; }
bool init_msc_is_writable_cb (uint8_t lun) { return false; }
bool init_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) { return false; }
bool init_msc_test_unit_ready_cb(uint8_t lun) { return false; }
void init_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {}
int32_t init_msc_scsi_cb(uint8_t lun, const uint8_t scsi_cmd[16], void *buffer, uint16_t bufsize) {return -1;}
int32_t init_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {return -1;}
int32_t init_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) { return -1;}
void init_msc_write10_complete_cb(uint8_t lun) {}

#else

// If there are multiple SCSI devices connected, they are mapped into LUNs for host.
static struct {
    int target_id;
    uint32_t sectorsize;
    uint32_t sectorcount;
} g_msc_initiator_targets[NUM_SCSIID];
static int g_msc_initiator_target_count;

static void scan_targets()
{
    int initiator_id = scsiInitiatorGetOwnID();
    uint8_t inquiry_data[36] = {0};
    g_msc_initiator_target_count = 0;
    for (int target_id = 0; target_id < NUM_SCSIID; target_id++)
    {
        if (target_id == initiator_id) continue;

        if (scsiTestUnitReady(target_id))
        {
            uint32_t sectorcount, sectorsize;

            bool inquiryok =
                scsiStartStopUnit(target_id, true) &&
                scsiInquiry(target_id, inquiry_data) &&
                scsiInitiatorReadCapacity(target_id, &sectorcount, &sectorsize);

            char vendor_id[9] = {0};
            char product_id[17] = {0};
            memcpy(vendor_id, &inquiry_data[8], 8);
            memcpy(product_id, &inquiry_data[16], 16);

            if (inquiryok)
            {
                logmsg("Found SCSI drive with ID ", target_id, ": ", vendor_id, " ", product_id);
                g_msc_initiator_targets[g_msc_initiator_target_count].target_id = target_id;
                g_msc_initiator_targets[g_msc_initiator_target_count].sectorcount = sectorcount;
                g_msc_initiator_targets[g_msc_initiator_target_count].sectorsize = sectorsize;
                g_msc_initiator_target_count++;
            }
            else
            {
                logmsg("Detected SCSI device with ID ", target_id, ", but failed to get inquiry response, skipping");
            }
        }
    }
}

bool setup_msc_initiator()
{
    logmsg("SCSI Initiator: activating USB MSC mode");
    g_msc_initiator = true;

    scsiInitiatorInit();

    // Scan for targets
    scan_targets();

    logmsg("SCSI Initiator: found " , g_msc_initiator_target_count, " SCSI drives");
    return g_msc_initiator_target_count > 0;
}

void poll_msc_initiator()
{
    if (g_msc_initiator_target_count == 0)
    {
        // Scan for targets until we find one
        scan_targets();
    }
}

static int get_target(uint8_t lun)
{
    if (lun >= g_msc_initiator_target_count)
    {
        logmsg("Host requested access to non-existing lun ", (int)lun);
        return 0;
    }
    else
    {
        return g_msc_initiator_targets[lun].target_id;
    }
}

void init_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
{
    LED_ON();

    int target = get_target(lun);
    uint8_t response[36] = {0};
    bool status = scsiInquiry(target, response);
    if (!status)
    {
        logmsg("SCSI Inquiry to target ", target, " failed");
    }

    memcpy(vendor_id, &response[8], 8);
    memcpy(product_id, &response[16], 16);
    memcpy(product_rev, &response[32], 4);

    LED_OFF();
}

uint8_t init_msc_get_maxlun_cb(void)
{
    return g_msc_initiator_target_count;
}

bool init_msc_is_writable_cb (uint8_t lun)
{
    int target = get_target(lun);
    uint8_t command[6] = {0x1A, 0x08, 0, 0, 4, 0}; // MODE SENSE(6)
    uint8_t response[4] = {0};
    scsiInitiatorRunCommand(target, command, 6, response, 4, NULL, 0);
    return (response[2] & 0x80) == 0; // Check write protected bit
}

bool init_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    LED_ON();

    int target = get_target(lun);
    uint8_t command[6] = {0x1B, 0x1, 0, 0, 0, 0};
    uint8_t response[4] = {0};
    
    if (start)
    {
        command[4] |= 1; // Start
        command[1] = 0;  // Immediate
    }

    if (load_eject)
    {
        command[4] |= 2;
    }

    command[4] |= power_condition << 4;

    int status = scsiInitiatorRunCommand(target,
                                         command, sizeof(command),
                                         response, sizeof(response),
                                         NULL, 0);

    if (status == 2)
    {
        uint8_t sense_key;
        scsiRequestSense(target, &sense_key);
        logmsg("START STOP UNIT on target ", target, " failed, sense key ", sense_key);
    }

    LED_OFF();

    return status == 0;
}

bool init_msc_test_unit_ready_cb(uint8_t lun)
{
    return scsiTestUnitReady(get_target(lun));
}

void init_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    uint32_t sectorcount = 0;
    uint32_t sectorsize = 0;
    scsiInitiatorReadCapacity(get_target(lun), &sectorcount, &sectorsize);
    *block_count = sectorcount;
    *block_size = sectorsize;
}

int32_t init_msc_scsi_cb(uint8_t lun, const uint8_t scsi_cmd[16], void *buffer, uint16_t bufsize)
{
    // NOTE: the TinyUSB API around free-form commands is not very good,
    // this function could need improvement.
    
    // Figure out command length
    static const uint8_t CmdGroupBytes[8] = {6, 10, 10, 6, 16, 12, 6, 6}; // From SCSI2SD
    int cmdlen = CmdGroupBytes[scsi_cmd[0] >> 5];

    int target = get_target(lun);
    int status = scsiInitiatorRunCommand(target,
                                         scsi_cmd, cmdlen,
                                         NULL, 0,
                                         (const uint8_t*)buffer, bufsize);

    return status;
}

int32_t init_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize)
{
    LED_ON();

    int status = -1;

    int target_id = get_target(lun);
    int sectorsize = g_msc_initiator_targets[lun].sectorsize;
    uint32_t start_sector = lba;
    uint32_t sectorcount = bufsize / sectorsize;

    if (sectorcount == 0)
    {
        // Not enough buffer left for a full sector
        return 0;
    }

    // Read6 command supports 21 bit LBA - max of 0x1FFFFF
    // ref: https://www.seagate.com/files/staticfiles/support/docs/manual/Interface%20manuals/100293068j.pdf pg 134
    if (start_sector < 0x1FFFFF && sectorcount <= 256)
    {
        // Use READ6 command for compatibility with old SCSI1 drives
        uint8_t command[6] = {0x08,
            (uint8_t)(start_sector >> 16),
            (uint8_t)(start_sector >> 8),
            (uint8_t)start_sector,
            (uint8_t)sectorcount,
            0x00
        };

        status = scsiInitiatorRunCommand(target_id, command, sizeof(command), (uint8_t*)buffer, bufsize, NULL, 0);
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

        status = scsiInitiatorRunCommand(target_id, command, sizeof(command), (uint8_t*)buffer, bufsize, NULL, 0);
    }

    LED_OFF();

    if (status != 0)
    {
        uint8_t sense_key;
        scsiRequestSense(target_id, &sense_key);

        logmsg("SCSI Initiator read failed: ", status, " sense key ", sense_key);
        return -1;
    }

    return sectorcount * sectorsize;
}

int32_t init_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    int status = -1;

    int target_id = get_target(lun);
    int sectorsize = g_msc_initiator_targets[lun].sectorsize;
    uint32_t start_sector = lba;
    uint32_t sectorcount = bufsize / sectorsize;

    if (sectorcount == 0)
    {
        // Not a complete sector
        return 0;
    }

    LED_ON();

    // Write6 command supports 21 bit LBA - max of 0x1FFFFF
    if (start_sector < 0x1FFFFF && sectorcount <= 256)
    {
        // Use WRITE6 command for compatibility with old SCSI1 drives
        uint8_t command[6] = {0x0A,
            (uint8_t)(start_sector >> 16),
            (uint8_t)(start_sector >> 8),
            (uint8_t)start_sector,
            (uint8_t)sectorcount,
            0x00
        };

        status = scsiInitiatorRunCommand(target_id, command, sizeof(command), NULL, 0, buffer, bufsize);
    }
    else
    {
        // Use READ10 command for larger number of blocks
        uint8_t command[10] = {0x2A, 0x00,
            (uint8_t)(start_sector >> 24), (uint8_t)(start_sector >> 16),
            (uint8_t)(start_sector >> 8), (uint8_t)start_sector,
            0x00,
            (uint8_t)(sectorcount >> 8), (uint8_t)(sectorcount),
            0x00
        };

        status = scsiInitiatorRunCommand(target_id, command, sizeof(command), NULL, 0, buffer, bufsize);
    }

    LED_OFF();

    if (status != 0)
    {
        uint8_t sense_key;
        scsiRequestSense(target_id, &sense_key);

        logmsg("SCSI Initiator write failed: ", status, " sense key ", sense_key);
        return -1;
    }

    return sectorcount * sectorsize;
}

void init_msc_write10_complete_cb(uint8_t lun)
{
    (void)lun;
}


#endif