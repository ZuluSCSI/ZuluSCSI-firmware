/**
 * ZuluSCSI™ - Copyright (c) 2025 Rabbit Hole Computing™
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 **/

#ifdef PLATFORM_MASS_STORAGE
#ifndef PIO_FRAMEWORK_ARDUINO_NO_USB

#include "ZuluSCSI_usb_console_media.h"
#include "ZuluSCSI_platform.h"
#include "ZuluSCSI_config.h"
#include <ZuluSCSI_control_api.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

extern "C" {
#include <scsi2sd.h>
}

// -----------------------------------------------------------------------
// Direct serial output helpers — bypass the log buffer entirely so that
// interactive menu text does not pollute zululog.txt.
// -----------------------------------------------------------------------

static void serial_out(const char *str)
{
    if (!str || !*str) return;
    uint32_t remaining = (uint32_t)strlen(str);
    const uint8_t *p = (const uint8_t *)str;
    while (remaining > 0)
    {
        uint32_t sent = platform_write_to_serial((uint8_t *)p, remaining);
        if (sent == 0) break;
        p += sent;
        remaining -= sent;
    }
}

static void serial_out_int(int value)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", value);
    serial_out(buf);
}

// Convenience: print a string followed by \r\n
static void serial_println(const char *str)
{
    serial_out(str);
    serial_out("\r\n");
}

// -----------------------------------------------------------------------
// State machine
// -----------------------------------------------------------------------

typedef enum
{
    MEDIA_MENU_INACTIVE,
    MEDIA_MENU_DEVICE_LIST,    // Waiting for device index digit or 'b'
    MEDIA_MENU_DEVICE_ACTIONS, // Waiting for action key for selected device
    MEDIA_MENU_IMAGE_LIST,     // Waiting for image index digits + Enter or 'b'
} media_menu_state_t;

static media_menu_state_t s_state = MEDIA_MENU_INACTIVE;

static uint8_t s_removable_ids[S2S_MAX_TARGETS];
static int     s_removable_count = 0;
static int     s_selected_idx = -1;
static int     s_image_count = 0;

// Up to 3 digits for image index entry
static char s_num_buf[4];
static int  s_num_len = 0;

// -----------------------------------------------------------------------
// Display helpers
// -----------------------------------------------------------------------

static void show_device_list()
{
    serial_println("");
    serial_println("  Media Management");
    serial_println("  ================================================");
    serial_println("  Removable devices:");

    if (s_removable_count == 0)
    {
        serial_println("    (none configured)");
    }
    else
    {
        for (int i = 0; i < s_removable_count; i++)
        {
            uint8_t id = s_removable_ids[i];
            char cur[MAX_FILE_PATH];
            bool has_img = controlGetCurrentImage(id, cur, sizeof(cur));

            const char *basename = "(ejected)";
            if (has_img)
            {
                const char *slash = strrchr(cur, '/');
                basename = slash ? slash + 1 : cur;
            }

            serial_out("    ");
            serial_out_int(i);
            serial_out(": ID ");
            serial_out_int((int)id);
            serial_out("  ");
            serial_out(controlGetDeviceTypeName(id));
            serial_out("  [");
            serial_out(basename);
            serial_println("]");
        }
    }

    serial_println("  ------------------------------------------------");
    serial_out("  Select device (0-");
    serial_out_int(s_removable_count - 1);
    serial_println(") or 'b' to exit:");
}

static void show_device_actions()
{
    uint8_t id = s_removable_ids[s_selected_idx];
    char cur[MAX_FILE_PATH];
    bool has_img = controlGetCurrentImage(id, cur, sizeof(cur));
    const char *basename = "(ejected)";
    if (has_img)
    {
        const char *slash = strrchr(cur, '/');
        basename = slash ? slash + 1 : cur;
    }

    serial_println("");
    serial_out("  Device SCSI ID ");
    serial_out_int((int)id);
    serial_out(": ");
    serial_println(controlGetDeviceTypeName(id));
    serial_out("  Current: ");
    serial_println(basename);
    serial_println("  ================================================");
    serial_println("    'l' - list and select image");
    serial_println("    'n' - load next image");
    serial_println("    'e' - eject");
    serial_println("    'i' - insert");
    serial_println("    'b' - back to device list");
    serial_println("  Or enter image number + Enter to load directly:");
}

// Callback used by controlListImages() to print each entry
static void print_image_cb(int idx, const char *filename,
                           const char *full_path, uint64_t size, bool is_dir, void *ud)
{
    (void)full_path;
    (void)ud;
    uint32_t size_mb = (uint32_t)((size + 524288ULL) / 1048576ULL);
    serial_out("    ");
    serial_out_int(idx + 1);
    serial_out(": ");
    serial_out(filename);
    serial_out(is_dir ? " (bin/cue " : " (");
    serial_out_int((int)size_mb);
    serial_println(" MB)");
}

static void show_image_list()
{
    uint8_t id = s_removable_ids[s_selected_idx];

    serial_println("");
    serial_out("  Available images for SCSI ID ");
    serial_out_int((int)id);
    serial_out(" (");
    serial_out(controlGetDeviceTypeName(id));
    serial_println("):");
    serial_println("  ------------------------------------------------");

    s_image_count = controlListImages(id, print_image_cb, nullptr);

    if (s_image_count == 0)
        serial_println("    (no images found)");

    serial_println("  ------------------------------------------------");
    serial_println("  Enter image number then Enter, or 'b' to cancel:");
}

// Callback to resolve the Nth image to a full path
struct FindNthState
{
    int  target;
    char path[MAX_FILE_PATH];
    bool found;
};
static FindNthState s_findNth;

static void find_nth_cb(int idx, const char *filename,
                        const char *full_path, uint64_t size, bool is_dir, void *ud)
{
    (void)filename;
    (void)size;
    (void)is_dir;
    (void)ud;
    if (idx == s_findNth.target && !s_findNth.found)
    {
        strncpy(s_findNth.path, full_path, MAX_FILE_PATH - 1);
        s_findNth.path[MAX_FILE_PATH - 1] = '\0';
        s_findNth.found = true;
    }
}

// -----------------------------------------------------------------------
// Public interface
// -----------------------------------------------------------------------

extern "C" bool serialMediaMenuActive()
{
    return s_state != MEDIA_MENU_INACTIVE;
}

extern "C" void serialMediaMenuEnter()
{
    s_removable_count = controlListRemovableDevices(s_removable_ids,
                                                    S2S_MAX_TARGETS);
    s_selected_idx = -1;
    s_num_len = 0;
    s_num_buf[0] = '\0';
    s_state = MEDIA_MENU_DEVICE_LIST;
    show_device_list();
}

extern "C" void serialMediaMenuProcess(char c)
{
    // Ignore bare CR/LF when no digits are pending
    if ((c == '\r' || c == '\n') && s_num_len == 0)
        return;

    switch (s_state)
    {
        // ------------------------------------------------------------
        case MEDIA_MENU_DEVICE_LIST:
        {
            if (c == 'b' || c == 'B')
            {
                serial_println("  Exiting media menu.");
                s_state = MEDIA_MENU_INACTIVE;
                return;
            }
            int dev_idx = c - '0';
            if (c >= '0' && dev_idx < s_removable_count)
            {
                s_selected_idx = dev_idx;
                s_state = MEDIA_MENU_DEVICE_ACTIONS;
                show_device_actions();
                return;
            }
            show_device_list();
            break;
        }

        // ------------------------------------------------------------
        case MEDIA_MENU_DEVICE_ACTIONS:
        {
            uint8_t id = s_removable_ids[s_selected_idx];

            if (c >= '0' && c <= '9' && s_num_len < 3)
            {
                s_num_buf[s_num_len++] = c;
                s_num_buf[s_num_len] = '\0';
                serial_out("  >> ");
                serial_out(s_num_buf);
                serial_println(" (press Enter to load, 'b' to cancel)");
                break;
            }

            if ((c == '\r' || c == '\n') && s_num_len > 0)
            {
                int image_idx = atoi(s_num_buf) - 1;
                s_num_len = 0;
                s_num_buf[0] = '\0';

                s_findNth.target = image_idx;
                s_findNth.found  = false;
                s_findNth.path[0] = '\0';
                controlListImages(id, find_nth_cb, nullptr);

                if (!s_findNth.found)
                {
                    serial_out("  Image ");
                    serial_out_int(image_idx + 1);
                    serial_println(" not found.");
                }
                else
                {
                    serial_out("  Loading '");
                    serial_out(s_findNth.path);
                    serial_out("' on SCSI ID ");
                    serial_out_int((int)id);
                    serial_println(" ...");

                    if (controlLoadImage(id, s_findNth.path))
                        serial_println("  Image loaded. Host will see a media change.");
                    else
                        serial_println("  Failed to load image.");
                }
                show_device_actions();
                break;
            }

            switch (c)
            {
                case 'b':
                case 'B':
                    s_num_len = 0;
                    s_num_buf[0] = '\0';
                    s_state = MEDIA_MENU_DEVICE_LIST;
                    s_removable_count = controlListRemovableDevices(
                                            s_removable_ids, S2S_MAX_TARGETS);
                    show_device_list();
                    break;

                case 'l':
                case 'L':
                    s_num_len = 0;
                    s_num_buf[0] = '\0';
                    s_state = MEDIA_MENU_IMAGE_LIST;
                    show_image_list();
                    break;

                case 'n':
                case 'N':
                    if (controlLoadImage(id, nullptr))
                        serial_println("  Next image staged — host will see media change.");
                    else
                        serial_println("  No next image available.");
                    show_device_actions();
                    break;

                case 'e':
                case 'E':
                {
                    char id_str[4];
                    snprintf(id_str, sizeof(id_str), "%d", (int)id);
                    if (controlEjectMedia(id))
                    {
                        serial_out("  SCSI ID ");
                        serial_out(id_str);
                        serial_println(" ejected.");
                    }
                    else
                    {
                        serial_println("  Eject failed.");
                    }
                    show_device_actions();
                    break;
                }

                case 'i':
                case 'I':
                {
                    char id_str[4];
                    snprintf(id_str, sizeof(id_str), "%d", (int)id);
                    if (controlInsertMedia(id))
                    {
                        serial_out("  SCSI ID ");
                        serial_out(id_str);
                        serial_println(" media inserted.");
                    }
                    else
                    {
                        serial_println("  Insert failed.");
                    }
                    show_device_actions();
                    break;
                }

                default:
                    s_num_len = 0;
                    s_num_buf[0] = '\0';
                    show_device_actions();
                    break;
            }
            break;
        }

        // ------------------------------------------------------------
        case MEDIA_MENU_IMAGE_LIST:
        {
            if (c == 'b' || c == 'B')
            {
                s_num_len = 0;
                s_state = MEDIA_MENU_DEVICE_ACTIONS;
                show_device_actions();
                return;
            }

            if (c >= '0' && c <= '9' && s_num_len < 3)
            {
                s_num_buf[s_num_len++] = c;
                s_num_buf[s_num_len] = '\0';
                serial_out("  >> ");
                serial_out(s_num_buf);
                serial_println(" (press Enter to load, 'b' to cancel)");
                return;
            }

            if ((c == '\r' || c == '\n') && s_num_len > 0)
            {
                int image_idx = atoi(s_num_buf);
                s_num_len = 0;
                s_num_buf[0] = '\0';

                if (image_idx < 1 || image_idx > s_image_count)
                {
                    serial_out("  Invalid index ");
                    serial_out_int(image_idx);
                    serial_out(" (valid range 1-");
                    serial_out_int(s_image_count);
                    serial_println(").");
                    serial_println("  Enter image number then Enter, or 'b' to cancel:");
                    return;
                }

                s_findNth.target = image_idx - 1;
                s_findNth.found  = false;
                s_findNth.path[0] = '\0';
                uint8_t id = s_removable_ids[s_selected_idx];
                controlListImages(id, find_nth_cb, nullptr);

                if (!s_findNth.found)
                {
                    serial_out("  Could not locate image at index ");
                    serial_out_int(image_idx);
                    serial_println(".");
                    serial_println("  Enter image number then Enter, or 'b' to cancel:");
                    return;
                }

                serial_out("  Loading '");
                serial_out(s_findNth.path);
                serial_out("' on SCSI ID ");
                serial_out_int((int)id);
                serial_println(" ...");

                if (controlLoadImage(id, s_findNth.path))
                    serial_println("  Image loaded. Host will see a media change.");
                else
                    serial_println("  Failed to load image.");

                s_state = MEDIA_MENU_DEVICE_ACTIONS;
                show_device_actions();
                return;
            }

            serial_println("  Enter image number then Enter, or 'b' to cancel:");
            break;
        }

        default:
            s_state = MEDIA_MENU_INACTIVE;
            break;
    }
}

#endif // PIO_FRAMEWORK_ARDUINO_NO_USB
#endif // PLATFORM_MASS_STORAGE
