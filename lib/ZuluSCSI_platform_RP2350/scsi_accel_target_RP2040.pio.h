// -------------------------------------------------- //
// This file is autogenerated by pioasm; do not edit! //
// -------------------------------------------------- //

#pragma once

#if !PICO_NO_HARDWARE
#include "hardware/pio.h"
#endif

// ----------- //
// scsi_parity //
// ----------- //

#define scsi_parity_wrap_target 0
#define scsi_parity_wrap 3

static const uint16_t scsi_parity_program_instructions[] = {
            //     .wrap_target
    0x80a0, //  0: pull   block                      
    0x4061, //  1: in     null, 1                    
    0x40e8, //  2: in     osr, 8                     
    0x4037, //  3: in     x, 23                      
            //     .wrap
};

#if !PICO_NO_HARDWARE
static const struct pio_program scsi_parity_program = {
    .instructions = scsi_parity_program_instructions,
    .length = 4,
    .origin = -1,
};

static inline pio_sm_config scsi_parity_program_get_default_config(uint offset) {
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + scsi_parity_wrap_target, offset + scsi_parity_wrap);
    return c;
}
#endif

// ---------------------- //
// scsi_accel_async_write //
// ---------------------- //

#define scsi_accel_async_write_wrap_target 0
#define scsi_accel_async_write_wrap 4

static const uint16_t scsi_accel_async_write_program_instructions[] = {
            //     .wrap_target
    0x90e0, //  0: pull   ifempty block   side 1     
    0x7009, //  1: out    pins, 9         side 1     
    0x7577, //  2: out    null, 23        side 1 [5] 
    0x308a, //  3: wait   1 gpio, 10      side 1     
    0x200a, //  4: wait   0 gpio, 10      side 0     
            //     .wrap
};

#if !PICO_NO_HARDWARE
static const struct pio_program scsi_accel_async_write_program = {
    .instructions = scsi_accel_async_write_program_instructions,
    .length = 5,
    .origin = -1,
};

static inline pio_sm_config scsi_accel_async_write_program_get_default_config(uint offset) {
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + scsi_accel_async_write_wrap_target, offset + scsi_accel_async_write_wrap);
    sm_config_set_sideset(&c, 1, false, false);
    return c;
}
#endif

// --------------- //
// scsi_accel_read //
// --------------- //

#define scsi_accel_read_wrap_target 0
#define scsi_accel_read_wrap 5

static const uint16_t scsi_accel_read_program_instructions[] = {
            //     .wrap_target
    0x90a0, //  0: pull   block           side 1     
    0x308a, //  1: wait   1 gpio, 10      side 1     
    0x4061, //  2: in     null, 1         side 0     
    0x200a, //  3: wait   0 gpio, 10      side 0     
    0x5009, //  4: in     pins, 9         side 1     
    0x5056, //  5: in     y, 22           side 1     
            //     .wrap
};

#if !PICO_NO_HARDWARE
static const struct pio_program scsi_accel_read_program = {
    .instructions = scsi_accel_read_program_instructions,
    .length = 6,
    .origin = -1,
};

static inline pio_sm_config scsi_accel_read_program_get_default_config(uint offset) {
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + scsi_accel_read_wrap_target, offset + scsi_accel_read_wrap);
    sm_config_set_sideset(&c, 1, false, false);
    return c;
}
#endif

// --------------- //
// scsi_sync_write //
// --------------- //

#define scsi_sync_write_wrap_target 0
#define scsi_sync_write_wrap 2

static const uint16_t scsi_sync_write_program_instructions[] = {
            //     .wrap_target
    0x7009, //  0: out    pins, 9         side 1     
    0x6077, //  1: out    null, 23        side 0     
    0x5061, //  2: in     null, 1         side 1     
            //     .wrap
};

#if !PICO_NO_HARDWARE
static const struct pio_program scsi_sync_write_program = {
    .instructions = scsi_sync_write_program_instructions,
    .length = 3,
    .origin = -1,
};

static inline pio_sm_config scsi_sync_write_program_get_default_config(uint offset) {
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + scsi_sync_write_wrap_target, offset + scsi_sync_write_wrap);
    sm_config_set_sideset(&c, 1, false, false);
    return c;
}
#endif

// --------------------- //
// scsi_sync_write_pacer //
// --------------------- //

#define scsi_sync_write_pacer_wrap_target 0
#define scsi_sync_write_pacer_wrap 2

static const uint16_t scsi_sync_write_pacer_program_instructions[] = {
            //     .wrap_target
    0x208a, //  0: wait   1 gpio, 10                 
    0x200a, //  1: wait   0 gpio, 10                 
    0x6061, //  2: out    null, 1                    
            //     .wrap
};

#if !PICO_NO_HARDWARE
static const struct pio_program scsi_sync_write_pacer_program = {
    .instructions = scsi_sync_write_pacer_program_instructions,
    .length = 3,
    .origin = -1,
};

static inline pio_sm_config scsi_sync_write_pacer_program_get_default_config(uint offset) {
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + scsi_sync_write_pacer_wrap_target, offset + scsi_sync_write_pacer_wrap);
    return c;
}
#endif

// -------------------- //
// scsi_sync_read_pacer //
// -------------------- //

#define scsi_sync_read_pacer_wrap_target 0
#define scsi_sync_read_pacer_wrap 2

static const uint16_t scsi_sync_read_pacer_program_instructions[] = {
            //     .wrap_target
    0x9020, //  0: push   block           side 1     
    0x0040, //  1: jmp    x--, 0          side 0     
    0x1002, //  2: jmp    2               side 1     
            //     .wrap
};

#if !PICO_NO_HARDWARE
static const struct pio_program scsi_sync_read_pacer_program = {
    .instructions = scsi_sync_read_pacer_program_instructions,
    .length = 3,
    .origin = -1,
};

static inline pio_sm_config scsi_sync_read_pacer_program_get_default_config(uint offset) {
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + scsi_sync_read_pacer_wrap_target, offset + scsi_sync_read_pacer_wrap);
    sm_config_set_sideset(&c, 1, false, false);
    return c;
}
#endif

// ---------------- //
// scsi_read_parity //
// ---------------- //

#define scsi_read_parity_wrap_target 0
#define scsi_read_parity_wrap 4

static const uint16_t scsi_read_parity_program_instructions[] = {
            //     .wrap_target
    0x60c8, //  0: out    isr, 8                     
    0x8020, //  1: push   block                      
    0x6038, //  2: out    x, 24                      
    0x0040, //  3: jmp    x--, 0                     
    0xc000, //  4: irq    nowait 0                   
            //     .wrap
};

#if !PICO_NO_HARDWARE
static const struct pio_program scsi_read_parity_program = {
    .instructions = scsi_read_parity_program_instructions,
    .length = 5,
    .origin = -1,
};

static inline pio_sm_config scsi_read_parity_program_get_default_config(uint offset) {
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + scsi_read_parity_wrap_target, offset + scsi_read_parity_wrap);
    return c;
}
#endif
