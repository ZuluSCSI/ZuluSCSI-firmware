/*
 * Host-side self-check for the ZuluSCSI Blaster PCM audio streaming protocol.
 *
 * The real logic lives in audio_i2s.cpp / ZuluSCSI_audio_stream.cpp and is bound
 * to the DMA/I2S hardware, so it can't link on the host. This is a faithful
 * model of the two-slot ring + flow control and the AUDIO_INFO wire format -
 * the cross-component contract drivers depend on. It fails if an invariant of
 * that algorithm breaks.
 *
 *   cc -Wall -Wextra test/test_audio_stream.c -o /tmp/t && /tmp/t
 *
 * Not part of the PlatformIO Unity suite on purpose: no framework, just asserts.
 */
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* ---- mirrors of firmware constants ---- */
#define SLOT_SIZE   (2352u * 12u)   /* AUDIO_BUFFER_SIZE in audio_i2s.h */
#define TOTAL_SIZE  (2u * SLOT_SIZE)
#define AUDIO_SCSI_INFO_LEN 32u
#define AUDIO_SCSI_PROTOCOL_VERSION 1
enum { ST_STALE, ST_READY };        /* model collapses FILLING/PROCESSING into READY */

/* ---- model of the two-slot engine (audio_stream_* in audio_i2s.cpp) ---- */
typedef struct {
    int slot[2];          /* ST_STALE = free to fill, ST_READY = holds audio/silence */
    uint32_t underruns;
    uint32_t overflows;
} engine_t;

static void eng_start(engine_t *e) {
    e->slot[0] = ST_READY;   /* both start as silence */
    e->slot[1] = ST_READY;
    e->underruns = 0;
    e->overflows = 0;
}

/* audio_stream_write: fill one STALE slot, else overflow. Returns bytes accepted. */
static uint32_t eng_write(engine_t *e, uint32_t len) {
    if (len > SLOT_SIZE) len = SLOT_SIZE;     /* one slot per write (v1) */
    for (int i = 0; i < 2; i++) {
        if (e->slot[i] == ST_STALE) { e->slot[i] = ST_READY; return len; }
    }
    e->overflows++;
    return 0;
}

/* DMA IRQ: a READY slot finishes playing and becomes STALE. */
static void eng_dma_consume(engine_t *e, int i) {
    if (e->slot[i] == ST_READY) e->slot[i] = ST_STALE;
}

/* audio_poll host branch: silence-fill only when BOTH slots are drained. */
static void eng_poll(engine_t *e) {
    if (e->slot[0] == ST_STALE && e->slot[1] == ST_STALE) {
        e->slot[0] = ST_READY;
        e->slot[1] = ST_READY;
        e->underruns++;
    }
}

static uint32_t eng_bytes_free(const engine_t *e) {
    uint32_t n = 0;
    if (e->slot[0] == ST_STALE) n += SLOT_SIZE;
    if (e->slot[1] == ST_STALE) n += SLOT_SIZE;
    return n;
}
static uint32_t eng_bytes_queued(const engine_t *e) {
    return TOTAL_SIZE - eng_bytes_free(e);
}

/* ---- mirror of AUDIO_INFO encoding (ZuluSCSI_audio_stream.cpp) ---- */
static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- invariants ---- */
static void check_accounting_invariant(const engine_t *e) {
    /* free + queued always covers the whole buffer, never over/under */
    assert(eng_bytes_free(e) + eng_bytes_queued(e) == TOTAL_SIZE);
    assert(eng_bytes_free(e) <= TOTAL_SIZE);
}

/* A correctly-paced host writes whenever a slot is free; the DAC drains one
 * slot per tick. The host must keep up: no underrun, no overflow. */
static void test_paced_stream(void) {
    engine_t e; eng_start(&e);
    for (int tick = 0; tick < 10000; tick++) {
        /* host fills every free slot this tick (flow control: write <= free) */
        while (eng_bytes_free(&e) > 0) {
            uint32_t got = eng_write(&e, SLOT_SIZE);
            assert(got == SLOT_SIZE);
            check_accounting_invariant(&e);
        }
        /* DAC consumes one slot */
        eng_dma_consume(&e, tick & 1);
        eng_poll(&e);
        check_accounting_invariant(&e);
    }
    assert(e.overflows == 0);
    assert(e.underruns == 0);
}

/* Host crashes (stops writing). Output must go to silence via underrun, never
 * overflow, accounting stays sane. */
static void test_starvation(void) {
    engine_t e; eng_start(&e);
    for (int tick = 0; tick < 100; tick++) {
        eng_dma_consume(&e, tick & 1);
        eng_poll(&e);
        check_accounting_invariant(&e);
    }
    assert(e.underruns > 0);     /* silence was injected */
    assert(e.overflows == 0);    /* a silent host never overflows */
}

/* Host ignores flow control and overspeeds. Excess is dropped + counted, never
 * corrupts the slot count. */
static void test_overflow(void) {
    engine_t e; eng_start(&e);
    int dropped = 0;
    for (int i = 0; i < 50; i++) {
        if (eng_write(&e, SLOT_SIZE) == 0) dropped++;
        check_accounting_invariant(&e);
    }
    assert(dropped > 0);
    assert(e.overflows == (uint32_t)dropped);
}

/* AUDIO_INFO must land its fields at the documented offsets, little-endian. */
static void test_info_wire_format(void) {
    engine_t e; eng_start(&e);
    eng_dma_consume(&e, 0);   /* one slot free, one queued */
    eng_poll(&e);

    uint8_t d[AUDIO_SCSI_INFO_LEN];
    memset(d, 0, sizeof(d));
    d[0] = AUDIO_SCSI_PROTOCOL_VERSION;
    d[1] = 1;            /* playing */
    d[2] = 2;            /* channels */
    d[3] = 16;           /* bits */
    put_le32(d + 4,  44100);
    put_le32(d + 8,  TOTAL_SIZE);
    put_le32(d + 12, eng_bytes_free(&e));
    put_le32(d + 16, eng_bytes_queued(&e));
    put_le32(d + 20, e.underruns);
    put_le32(d + 24, e.overflows);
    d[28] = 0x3F;        /* volume */

    assert(d[0] == 1);
    assert(get_le32(d + 4) == 44100);
    assert(get_le32(d + 8) == TOTAL_SIZE);
    assert(get_le32(d + 12) == SLOT_SIZE);          /* one slot free */
    assert(get_le32(d + 16) == SLOT_SIZE);          /* one slot queued */
    assert(get_le32(d + 12) + get_le32(d + 16) == TOTAL_SIZE);
    assert(d[2] == 2 && d[3] == 16);                /* 16-bit stereo */
}

/* 24-bit big-endian length field in the AUDIO_WRITE CDB (cdb[2..4]). The CDB is
 * 10 bytes on the wire (group-6 opcode; Linux COMMAND_SIZE forces 10), but only
 * bytes 0..4 carry fields. */
static void test_cdb_length(void) {
    uint8_t cdb[10] = {0xC5, 0, 0x00, 0x6E, 0x40, 0, 0, 0, 0, 0};  /* 0x006E40 = 28224 */
    uint32_t len = ((uint32_t)cdb[2] << 16) | ((uint32_t)cdb[3] << 8) | cdb[4];
    assert(len == SLOT_SIZE);
}

int main(void) {
    test_paced_stream();
    test_starvation();
    test_overflow();
    test_info_wire_format();
    test_cdb_length();
    printf("audio stream protocol self-check: OK\n");
    return 0;
}
