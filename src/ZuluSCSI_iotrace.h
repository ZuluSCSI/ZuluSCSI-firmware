/**
 * Temporary I/O pattern instrumentation for the AS/400 performance
 * investigation (see project memory: project_as400_write_performance.md).
 *
 * Three fixed-size binary record types, appended directly to an open file
 * (same buffered-append/periodic-flush pattern as zululog.txt), gated by
 * `IOTrace=1` under `[SCSI]` so it costs nothing when off. Meant to be
 * ripped out once real evidence has been gathered -- not a permanent
 * feature, so simplicity is favored over polish throughout.
 *
 * Layer A (iotrace_cdb): one record per Read10/Write10/Skip CDB dispatch.
 * Layer B (iotrace_sd_access): one record per ImageBackingStore seek/
 *   read/write -- reveals Skip Write/Read's actual run structure for free,
 *   since skip_next()'s seeks and transfers both go through here.
 * Layer C (iotrace_loop_account / iotrace_loop_tick): accumulates time
 *   spent in each zuluscsi_main_loop() sub-call into buckets, emitting one
 *   summary record every IOTRACE_LOOP_EMIT_INTERVAL iterations rather than
 *   per-iteration -- an iteration count is a cheaper gate than a wall-clock
 *   comparison and the summary's own embedded timestamp is enough to
 *   reconstruct real elapsed time during offline analysis.
 *
 * Header-only, deliberately -- this project's src/build_bootloader.py
 * builds a second executable (bootloader.elf) by taking a snapshot of the
 * main build's already-compiled object files and swapping in a different
 * entry point. A separate ZuluSCSI_iotrace.cpp was tried first, but that
 * snapshot didn't reliably include a brand-new top-level .cpp file added
 * to src/ (confirmed: failed the same way even after a clean rebuild) -- rather
 * than risk changing that shared build script, every function here is
 * `inline` with function-local statics for its state, which the C++
 * standard guarantees resolve to a single shared instance across every
 * translation unit that includes this header, without needing a separate
 * .cpp file for anything to go missing from.
 *
 * All functions declared unconditionally below (real on PLATFORM_AS400,
 * no-op stubs elsewhere) -- ZuluSCSI.cpp and ImageBackingStore.cpp build
 * for every platform, not just AS/400, so nothing platform-specific may
 * leak into what those files see, including enum values used at call
 * sites: IOTraceLoopBucket is defined once, the same on every platform,
 * precisely so a caller never needs its own #ifdef PLATFORM_AS400 either.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

// Bucket identifiers for Layer C's per-call timing. DMA_WAIT is split out
// from DISK_COMPUTE specifically because it answers two questions at once:
// how much of scsiDiskPoll()'s time is genuine CPU work vs. spinning on
// SD DMA completion, and (the same number) how long the SCSI bus is held
// BSY while genuinely stalled rather than transferring -- see the
// "channel busy" discussion in project_as400_write_performance.md.
enum IOTraceLoopBucket : uint8_t
{
    IOTRACE_BUCKET_PLATFORM_POLL = 0,
    IOTRACE_BUCKET_SCSI_POLL,
    IOTRACE_BUCKET_DISK_COMPUTE,
    IOTRACE_BUCKET_DMA_WAIT,
    // Split out from OTHER, 2026-09-13: a first real capture (CISC,
    // Debug=0) showed OTHER at 34.7% of total time with zero attribution
    // -- these two are the sub-calls with a plausible reason to be
    // non-trivial (both do their own SD-card I/O), pulled out so OTHER
    // becomes a genuinely small residual instead of a catch-all guess.
    IOTRACE_BUCKET_SAVE_LOGFILE,
    IOTRACE_BUCKET_SD_MAINTENANCE,
    // Added 2026-09-13: two fresh real captures (both machines, both SD
    // cards) showed per-summary median iteration time roughly unchanged
    // from the pre-instrumentation baseline, but p95/p99 substantially
    // worse (Wide: p95 11.4us -> 50.2us, p99 674.7us; Blaster: p95 25.1us
    // -> 38.1us) -- user's own suspicion that the tracing itself is the
    // cause. Layer A/B/BOOT/IMAGEOPEN's iotrace_write() calls land
    // precisely on the busy (CDB/SD-access) iterations, which is where
    // the tail lives, not on the idle-loop median -- consistent with the
    // shape of the regression, but not yet *measured* directly. This
    // bucket wraps exactly those write() calls (see iotrace_cdb() /
    // iotrace_sd_access() / iotrace_imageopen() below) so the next
    // capture states the overhead as a number instead of an inference.
    // Deliberately overlaps with (is a subset of) SCSI_POLL/DISK_COMPUTE's
    // own totals -- not part of the OTHER-bucket subtraction in
    // zuluscsi_main_loop() -- since the point is to show what fraction of
    // time already counted there was actually just tracing itself.
    IOTRACE_BUCKET_TRACE_OVERHEAD,
    IOTRACE_BUCKET_OTHER,
    IOTRACE_BUCKET_COUNT
};

#define IOTRACE_SD_FLAG_WRITE       0x01
#define IOTRACE_SD_FLAG_CONTIGUOUS  0x02
#define IOTRACE_SD_FLAG_REVERTED    0x04

// IOTraceImageOpenRecord.flags
#define IOTRACE_IMAGEOPEN_FLAG_GOT_RANGE    0x01 // contiguousRange() succeeded at all
#define IOTRACE_IMAGEOPEN_FLAG_CONTIGUOUS   0x02 // ...and the range covers the whole file (fast path taken)

#ifdef PLATFORM_AS400

#include "ZuluSCSI.h"
#include "ZuluSCSI_log.h"
#include "ZuluSCSI_config.h"
#include "ZuluSCSI_settings.h"
#include <SdFat.h> // cid_t, for reading the SD card's own serial number
#include <minIni.h>
#include <Arduino.h> // pulls in the Pico SDK's pico/time.h, for time_us_64()

#define IOTRACE_FILE "iotrace.bin"

// How many main-loop iterations between Layer C summary emissions. See
// the file header comment above for why this is a count, not a timer.
#define IOTRACE_LOOP_EMIT_INTERVAL 2000

// One byte, at the start of every record, so a single file can hold all
// five record types (interleaved across a single boot, and concatenated
// across multiple boots -- iotrace.bin is never truncated, only appended
// to, so it can span an entire day's worth of separate test sessions)
// and still be parsed unambiguously.
enum IOTraceRecType : uint8_t
{
    IOTRACE_REC_CDB = 1,
    IOTRACE_REC_SDACCESS = 2,
    IOTRACE_REC_LOOP = 3,
    IOTRACE_REC_BOOT = 4,
    IOTRACE_REC_IMAGEOPEN = 5,
};

// Written once, the first time iotrace_init() runs after a genuine
// power-cycle (not an SD hotplug reinit within the same boot -- see
// iotrace_init()). Marks where a new boot's timestamps start, since
// time_us is only monotonic within one boot; offline analysis must treat
// every record between one BOOT marker and the next as its own separate
// time axis, not compare raw time_us values across a marker. sdCardSerial
// is the SD card's own CID product serial number (same value/pattern as
// as400_get_serial_8() in as400_values.cpp), added 2026-09-13 so a trace
// self-identifies which physical card it came from -- the investigation
// has already seen contradictory fast-path results across two different
// cards, and the card doesn't otherwise appear anywhere in iotrace.bin.
// 12 bytes.
struct __attribute__((packed)) IOTraceBootRecord
{
    uint8_t type; // IOTRACE_REC_BOOT
    uint8_t sysPreset;
    uint16_t _pad;
    uint32_t time_us; // usually near 0; recorded anyway for consistency
    uint32_t sdCardSerial; // SD card CID product serial number, 0 if unreadable
};

// Layer A. 16 bytes.
struct __attribute__((packed)) IOTraceCDBRecord
{
    uint8_t type; // IOTRACE_REC_CDB
    uint8_t scsiId;
    uint8_t sysPreset;
    uint8_t opcode;
    uint32_t time_us; // low 32 bits of time_us_64(), wraps ~71.5 min
    uint32_t lba;
    uint16_t blockCount;
    uint16_t _pad;
};

// Layer B. 20 bytes. durationUs added 2026-09-13: the actual wall-clock
// time of the underlying storage call this record describes (the same
// span Layer C pools into DMA_WAIT), so read/write latency can be
// analyzed per-access -- percentiles, read-vs-write split -- instead of
// only as one session-wide average pooled across every access.
struct __attribute__((packed)) IOTraceSDRecord
{
    uint8_t type; // IOTRACE_REC_SDACCESS
    uint8_t scsiId;
    uint8_t sysPreset;
    uint8_t flags; // IOTRACE_SD_FLAG_*
    uint32_t time_us;
    uint32_t sector;
    uint32_t durationUs;
    uint16_t sectorCount;
    uint16_t _pad;
};

// Emitted once per ImageBackingStore _internal_open() call (i.e. once per
// image file opened/reopened), added 2026-09-13 alongside sdCardSerial:
// the fast-path-availability decision was previously only visible in
// zululog.txt (plain logmsg(), see _internal_open()), meaning a trace file
// couldn't explain its own 0%-fast-path findings without cross-referencing
// a separate log from the same session. This puts the same decision
// directly into iotrace.bin. scsiId may be 0xFF (unknown) if the image is
// opened before setScsiId()/the constructor's scsiId argument has been
// given a real target -- see ImageBackingStore's constructor comment.
// bgnSector/endSector are only meaningful when flags has GOT_RANGE set.
// 20 bytes.
struct __attribute__((packed)) IOTraceImageOpenRecord
{
    uint8_t type; // IOTRACE_REC_IMAGEOPEN
    uint8_t scsiId;
    uint8_t sysPreset;
    uint8_t flags; // IOTRACE_IMAGEOPEN_FLAG_*
    uint32_t time_us;
    uint32_t bgnSector;
    uint32_t endSector;
    uint32_t sectorCount; // image's own sector count, for comparison against endSector-bgnSector+1
};

// Layer C. One summary per IOTRACE_LOOP_EMIT_INTERVAL main-loop
// iterations, not one per iteration. 12 + 4*IOTRACE_BUCKET_COUNT bytes
// (44 with the current 8 buckets) -- grows automatically if buckets are
// added/removed, since bucket_us[] is sized off the enum, not a literal.
struct __attribute__((packed)) IOTraceLoopRecord
{
    uint8_t type; // IOTRACE_REC_LOOP
    uint8_t sysPreset;
    uint16_t _pad;
    uint32_t time_us;
    uint32_t iterations;
    uint32_t bucket_us[IOTRACE_BUCKET_COUNT]; // indexed by IOTraceLoopBucket
};

// Function-local statics in a true `inline` (not `static inline`) function
// are guaranteed by the C++ standard to be a single instance shared across
// every translation unit that includes this header -- this is what makes
// the whole module safe to keep header-only. Accessed only from the
// functions below, never directly.
inline bool &iotrace_enabled_ref() { static bool v = false; return v; }
inline FsFile &iotrace_file_ref() { static FsFile v; return v; }
inline uint32_t &iotrace_loop_iterations_ref() { static uint32_t v = 0; return v; }
inline uint32_t *iotrace_loop_bucket_us_ref() { static uint32_t v[IOTRACE_BUCKET_COUNT] = {0}; return v; }

inline uint8_t iotrace_current_preset()
{
    return (uint8_t)g_scsi_settings.getSystemPreset();
}

inline void iotrace_write(const void *data, size_t len)
{
    if (!iotrace_enabled_ref()) return;
    if (!iotrace_file_ref().isOpen()) return;

    iotrace_file_ref().write((const uint8_t *)data, len);
}

// Portable "now" for Layer C callers to bracket a call with -- returns 0
// outside PLATFORM_AS400 (see the stub below), so a caller in a
// multi-platform file can always call this safely without its own #ifdef.
inline uint64_t iotrace_now_us() { return time_us_64(); }

// Layer C: add `elapsed_us` to `bucket`'s running total for this
// accounting window. Call once per (begin, end) pair around the
// corresponding zuluscsi_main_loop() sub-call or DMA-wait spin loop.
// Declared up here (ahead of iotrace_cdb() et al.) because Layer A/B/
// IMAGEOPEN's own IOTRACE_BUCKET_TRACE_OVERHEAD self-timing (see below)
// needs to call it too, and plain functions need their declaration
// before first use.
inline void iotrace_loop_account(IOTraceLoopBucket bucket, uint32_t elapsed_us)
{
    if (!iotrace_enabled_ref()) return;
    if (bucket >= IOTRACE_BUCKET_COUNT) return;

    iotrace_loop_bucket_us_ref()[bucket] += elapsed_us;
}

// Parses IOTrace= from [SCSI]. Mirrors how g_log_debug is parsed from
// Debug= in reinitSCSI() -- call from the same place.
inline void iotrace_load_setting()
{
    iotrace_enabled_ref() = ini_getbool("SCSI", "IOTrace", iotrace_enabled_ref(), CONFIGFILE);
}

// Call once at boot (after settings are loaded, so IOTrace= is known) and
// again on every SD card reinit, same lifecycle as init_logfile().
inline void iotrace_init()
{
    if (!iotrace_enabled_ref())
        return;

    // iotrace_init() is called from every init_logfile() call site (SD
    // hotplug reinit included, not just boot) -- if a trace is already
    // open and running from earlier this same boot, leave it alone.
    // Otherwise (including the first call after a genuine power-cycle)
    // open for append and write a fresh BOOT marker: iotrace.bin is never
    // truncated, so it can accumulate an entire day's separate test
    // sessions (each one distinguishable by its own BOOT marker) into one
    // file for a single consolidated analysis pass, instead of needing to
    // be manually copied off the SD card between every reboot.
    static bool first_call_this_boot = true;

    if (!first_call_this_boot && iotrace_file_ref().isOpen())
        return;

    first_call_this_boot = false;

    if (iotrace_file_ref().isOpen())
        iotrace_file_ref().close();

    iotrace_file_ref() = SD.open(IOTRACE_FILE, O_WRONLY | O_CREAT | O_APPEND);

    if (!iotrace_file_ref().isOpen())
    {
        logmsg("---- WARNING: could not open ", IOTRACE_FILE, ", IOTrace data will not be recorded");
        iotrace_enabled_ref() = false;
        return;
    }

    logmsg("---- IOTrace enabled, appending to ", IOTRACE_FILE);

    // Same pattern as as400_get_serial_8() in as400_values.cpp.
    uint32_t sd_serial = 0;
    cid_t sd_cid;
    if (SD.card()->readCID(&sd_cid))
        sd_serial = sd_cid.psn();

    IOTraceBootRecord boot_rec;
    boot_rec.type = IOTRACE_REC_BOOT;
    boot_rec.sysPreset = iotrace_current_preset();
    boot_rec._pad = 0;
    boot_rec.time_us = (uint32_t)time_us_64();
    boot_rec.sdCardSerial = sd_serial;
    iotrace_write(&boot_rec, sizeof(boot_rec));
    iotrace_file_ref().flush();

    iotrace_loop_iterations_ref() = 0;
    for (int i = 0; i < IOTRACE_BUCKET_COUNT; i++)
        iotrace_loop_bucket_us_ref()[i] = 0;
}

// Layer A: one CDB dispatch. blockCount is the CDB's own declared count
// (not sectors actually transferred -- Layer B shows what really happened).
inline void iotrace_cdb(uint8_t scsiId, uint8_t opcode, uint16_t blockCount, uint32_t lba)
{
    if (!iotrace_enabled_ref()) return;

    IOTraceCDBRecord rec;
    rec.type = IOTRACE_REC_CDB;
    rec.scsiId = scsiId;
    rec.sysPreset = iotrace_current_preset();
    rec.opcode = opcode;
    rec.time_us = (uint32_t)time_us_64();
    rec.lba = lba;
    rec.blockCount = blockCount;
    rec._pad = 0;

    uint64_t iotrace_t0 = iotrace_now_us();
    iotrace_write(&rec, sizeof(rec));
    iotrace_loop_account(IOTRACE_BUCKET_TRACE_OVERHEAD, (uint32_t)(iotrace_now_us() - iotrace_t0));
}

// Layer B: one ImageBackingStore access. `sector`/`sectorCount` are in the
// image's own sector units (matches m_cursector), not raw bytes -- keeps
// the record in 32 bits regardless of image size. `flags` bit 0 = write
// (0 = read/seek-only), bit 1 = m_iscontiguous at the time of the access,
// bit 2 = this access is the one that triggered revert_to_noncontiguous().
// `durationUs` is the wall-clock time of the underlying storage call this
// record describes (0 if not applicable, e.g. a ROM-drive access, which
// never touches the SD card at all).
inline void iotrace_sd_access(uint8_t scsiId, uint32_t sector, uint16_t sectorCount, uint8_t flags, uint32_t durationUs)
{
    if (!iotrace_enabled_ref()) return;

    IOTraceSDRecord rec;
    rec.type = IOTRACE_REC_SDACCESS;
    rec.scsiId = scsiId;
    rec.sysPreset = iotrace_current_preset();
    rec.flags = flags;
    rec.time_us = (uint32_t)time_us_64();
    rec.sector = sector;
    rec.durationUs = durationUs;
    rec.sectorCount = sectorCount;
    rec._pad = 0;

    uint64_t iotrace_t0 = iotrace_now_us();
    iotrace_write(&rec, sizeof(rec));
    iotrace_loop_account(IOTRACE_BUCKET_TRACE_OVERHEAD, (uint32_t)(iotrace_now_us() - iotrace_t0));
}

// One per ImageBackingStore _internal_open() call -- see
// IOTraceImageOpenRecord's own comment above for why this exists.
inline void iotrace_imageopen(uint8_t scsiId, uint32_t bgnSector, uint32_t endSector, uint32_t sectorCount, uint8_t flags)
{
    if (!iotrace_enabled_ref()) return;

    IOTraceImageOpenRecord rec;
    rec.type = IOTRACE_REC_IMAGEOPEN;
    rec.scsiId = scsiId;
    rec.sysPreset = iotrace_current_preset();
    rec.flags = flags;
    rec.time_us = (uint32_t)time_us_64();
    rec.bgnSector = bgnSector;
    rec.endSector = endSector;
    rec.sectorCount = sectorCount;

    uint64_t iotrace_t0 = iotrace_now_us();
    iotrace_write(&rec, sizeof(rec));
    iotrace_loop_account(IOTRACE_BUCKET_TRACE_OVERHEAD, (uint32_t)(iotrace_now_us() - iotrace_t0));
}

// Layer C: call once per zuluscsi_main_loop() iteration, after accounting
// for that iteration's buckets. Internally counts iterations and emits +
// resets the accumulators every IOTRACE_LOOP_EMIT_INTERVAL calls. Also
// where the periodic file flush is piggybacked (same gate, no separate
// timer needed).
inline void iotrace_loop_tick()
{
    if (!iotrace_enabled_ref()) return;

    uint32_t &iterations = iotrace_loop_iterations_ref();
    iterations++;

    if (iterations < IOTRACE_LOOP_EMIT_INTERVAL)
        return;

    IOTraceLoopRecord rec;
    rec.type = IOTRACE_REC_LOOP;
    rec.sysPreset = iotrace_current_preset();
    rec._pad = 0;
    rec.time_us = (uint32_t)time_us_64();
    rec.iterations = iterations;
    for (int i = 0; i < IOTRACE_BUCKET_COUNT; i++)
        rec.bucket_us[i] = iotrace_loop_bucket_us_ref()[i];

    iotrace_write(&rec, sizeof(rec));

    // Piggyback the periodic flush on the same gate -- no separate timer
    // needed, and this is already the lowest-frequency of the three record
    // types, so it's a natural place to pay the flush cost.
    if (iotrace_file_ref().isOpen())
        iotrace_file_ref().flush();

    iterations = 0;
    for (int i = 0; i < IOTRACE_BUCKET_COUNT; i++)
        iotrace_loop_bucket_us_ref()[i] = 0;
}

#else // !PLATFORM_AS400

// No-op elsewhere -- this is an AS/400-investigation-specific tool, not a
// general firmware feature.
inline uint64_t iotrace_now_us() { return 0; }
inline void iotrace_init() {}
inline void iotrace_load_setting() {}
inline void iotrace_cdb(uint8_t, uint8_t, uint16_t, uint32_t) {}
inline void iotrace_sd_access(uint8_t, uint32_t, uint16_t, uint8_t, uint32_t) {}
inline void iotrace_imageopen(uint8_t, uint32_t, uint32_t, uint32_t, uint8_t) {}
inline void iotrace_loop_account(IOTraceLoopBucket, uint32_t) {}
inline void iotrace_loop_tick() {}

#endif
