/**
 * ZuluSCSI Main Loop Hooks
 *
 * Integration hooks for the main ZuluSCSI loop.
 *
 * Copyright (c) 2025
 * Licensed under GPL-3.0
 *
 * =============================================================================
 * INTEGRATION INSTRUCTIONS
 * =============================================================================
 *
 * Add the following to ZuluSCSI.cpp:
 *
 * 1. Include the USB bridge header at the top:
 *
 *    #include "ZuluSCSI_usb_bridge.h"
 *
 * 2. In the init section (after platform_init()):
 *
 *    // Initialize USB bridge
 *    usb_bridge_init();
 *
 * 3. In the main loop, service USB when SCSI is idle:
 *
 *    void zuluscsi_main_loop() {
 *        platform_reset_watchdog();
 *        platform_poll();
 *
 *        // Service USB bridge when SCSI bus is free
 *        if (scsiDev.phase == BUS_FREE) {
 *            usb_bridge_poll();
 *        }
 *
 *        scsiPoll();
 *        // ... rest of loop
 *    }
 *
 * =============================================================================
 */

#include "ZuluSCSI_usb_bridge.h"
#include "ZuluSCSI_ram_overlay.h"

/**
 * Initialize USB bridge subsystem.
 * Call once during startup after platform_init().
 */
extern "C"
void usb_bridge_system_init(void)
{
    usb_bridge_init();
}

/**
 * Service USB bridge in main loop.
 * Call when SCSI bus is free.
 * Returns number of USB packets processed.
 */
extern "C"
int usb_bridge_main_poll(void)
{
    return usb_bridge_poll();
}

/**
 * Notify USB bridge of SD card removal.
 * Call when SD card is ejected.
 */
extern "C"
void usb_bridge_sd_removed(void)
{
    usb_bridge_send_notify_sd_removed();
}

/**
 * Notify USB bridge of SD card insertion.
 * Call when SD card is inserted.
 */
extern "C"
void usb_bridge_sd_inserted(void)
{
    usb_bridge_send_notify_sd_inserted();
}

/**
 * Notify USB bridge of SCSI bus reset.
 * Call when SCSI bus is reset.
 */
extern "C"
void usb_bridge_bus_reset(void)
{
    usb_bridge_send_notify_scsi_bus_reset();
}
