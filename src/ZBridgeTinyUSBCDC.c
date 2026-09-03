/*
 * Build TinyUSB's CDC device class as a project object so ZBridge can use the
 * receive FIFO selected in its project-local tusb_config.h. Arduino-Pico
 * otherwise supplies this object from a precompiled libpico.a, permanently
 * fixing the receive queue at 256 bytes regardless of project build flags.
 *
 * cdc_device.c is part of the pinned Arduino-Pico/TinyUSB framework and is
 * distributed under the MIT license. Defining its symbols here prevents the
 * linker from extracting the incompatible precompiled object.
 */
#include "class/cdc/cdc_device.c"
