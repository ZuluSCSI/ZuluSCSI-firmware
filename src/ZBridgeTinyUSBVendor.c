/*
 * Compile TinyUSB's vendor device class with the project-local configuration.
 * Arduino-Pico's prebuilt core omits this object when CFG_TUD_VENDOR is zero;
 * keeping this wrapper beside the CDC wrapper makes the ZBridge build
 * deterministic without patching the installed framework.
 */

#include <stdbool.h>

/*
 * The endpoint descriptor still advertises the USB full-speed maximum packet
 * size (64 bytes). Give TinyUSB a larger controller transfer window, though,
 * so the RP2350 DCD rearms consecutive 64-byte OUT packets in its IRQ path.
 * With a 64-byte window the vendor class must round-trip through tud_task()
 * after every packet, leaving the endpoint NAKed for much of each frame.
 */
#undef CFG_TUD_VENDOR_EPSIZE
#define CFG_TUD_VENDOR_EPSIZE 1024

// TinyUSB normally emits a ZLP whenever an IN controller window drains before
// the next window is queued. That correctly terminates a short application
// message, but it would terminate macOS's larger continuous PCM request after
// the first 1 KiB SCSI window. Rename the class driver's call so ZBridge can
// suppress only intermediate ZLPs while a negotiated native DATA IN stream is
// active. Exact host request lengths still terminate the final transfer.
extern bool zbridge_native_in_stream_active(void);
#define tu_edpt_stream_write_zlp_if_needed zbridge_vendor_write_zlp_if_needed
#include "class/vendor/vendor_device.c"
#undef tu_edpt_stream_write_zlp_if_needed

extern bool tu_edpt_stream_write_zlp_if_needed(
    uint8_t hwid, tu_edpt_stream_t *stream, uint32_t last_xferred_bytes);

bool zbridge_vendor_write_zlp_if_needed(
    uint8_t hwid, tu_edpt_stream_t *stream, uint32_t last_xferred_bytes)
{
    if (zbridge_native_in_stream_active()) return false;
    return tu_edpt_stream_write_zlp_if_needed(
        hwid, stream, last_xferred_bytes);
}

/*
 * Arduino-Pico supplies one application class driver for its reset interface.
 * Its precompiled TinyUSB core was built with CFG_TUD_VENDOR=0, so merely
 * linking vendord_* is insufficient: usbd never asks it to open our new
 * interface. The linker's --wrap hook preserves Arduino's reset driver and
 * appends the project-local vendor driver to the same application-driver list.
 */
extern usbd_class_driver_t const *
    __real_usbd_app_driver_get_cb(uint8_t *driver_count) __attribute__((weak));

static usbd_class_driver_t zbridge_app_drivers[2];

usbd_class_driver_t const *
__wrap_usbd_app_driver_get_cb(uint8_t *driver_count)
{
    uint8_t original_count = 0;
    usbd_class_driver_t const *original = NULL;
    if (__real_usbd_app_driver_get_cb)
    {
        original = __real_usbd_app_driver_get_cb(&original_count);
    }
    usbd_class_driver_t vendor_driver = (usbd_class_driver_t) {
        .name = "ZBridge Bulk",
        .init = vendord_init,
        .deinit = vendord_deinit,
        .reset = vendord_reset,
        .open = vendord_open,
        .control_xfer_cb = tud_vendor_control_xfer_cb,
        .xfer_cb = vendord_xfer_cb,
        .sof = NULL,
    };

    if (original_count > 0 && original)
    {
        zbridge_app_drivers[0] = original[0];
        zbridge_app_drivers[1] = vendor_driver;
        *driver_count = 2;
    }
    else
    {
        zbridge_app_drivers[0] = vendor_driver;
        *driver_count = 1;
    }
    return zbridge_app_drivers;
}
