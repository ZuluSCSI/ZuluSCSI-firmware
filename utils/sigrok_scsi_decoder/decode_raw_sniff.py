#!/usr/bin/env python3
"""
Standalone decoder for ZuluSCSI's RP2350 bus sniffer raw capture format
(zuluscsi_sniff.dat), without requiring sigrok/PulseView.

Format reference: .pio/libdeps/*/RP2350_Logic_Sniffer/lib/RP2350_Logic_Sniffer/rp2350_sniffer.pio
(the .pio file's own header comment is the authoritative spec; this
reimplements it directly against the raw byte stream, cross-verified
against this specific capture's own header bytes).

Word format (32-bit, little-endian):
    Bits 31..27: D (5-bit short time delta, or 31 = special)
    Bits 26..0:  P (pin states, or long time delta / special payload)

    D != 31            -> P = new 27-bit pin state. Cycles since previous
                           sample = 3 * (32 - D).
    D == 31, P<=0x03FFFFFF -> long time delta, pins unchanged.
                           Cycles = 3 * (0x03FFFFFF - P + 3)
    D == 31, P==0x07FFFFFF -> max time delta reached (~1s @ 200MHz)
    D == 31, otherwise  -> special/aux data item, sub-typed by (P>>24):
        4: timestamp, ms = P & 0xFFFFFF
        7: further split by P & 0xFFFFFF:
            bit23 set -> aux data header: data_id=(rem>>16)&0xF, len=rem&0xFFFF
            (rem>>16)==1 -> CPU clock speed in MHz = rem & 0xFFFF
            (rem>>16)==0 -> format version = rem & 0xFFFF
"""
import sys
import struct

DATAID_NAMES = {0: "IO_PINS", 1: "SYSLOG"}

def parse(path, max_events=None):
    with open(path, 'rb') as f:
        data = f.read()

    assert data[:20] == b"RP2350_Logic_Sniffer", "bad magic"
    pos = 20
    n = len(data)

    pin_names = []
    cpu_hz = None
    fmt_version = None
    cycle_accum = 0
    last_pins = None
    events = []  # (elapsed_us, kind, payload)

    # State for reassembling aux data streams (pin names, syslog text)
    aux_pending = {}  # data_id -> bytearray remaining bytes expected
    aux_buf = {0: bytearray(), 1: bytearray()}

    def cycles_to_us(cycles):
        if not cpu_hz:
            return None
        return cycles * 1_000_000.0 / cpu_hz

    word_count = 0
    while pos + 4 <= n:
        w = struct.unpack_from('<I', data, pos)[0]
        pos += 4
        word_count += 1
        D = (w >> 27) & 0x1F
        P = w & 0x07FFFFFF

        if D != 31:
            delta_cycles = 3 * (32 - D)
            cycle_accum += delta_cycles
            last_pins = P
            events.append((cycles_to_us(cycle_accum), 'pins', P))
        else:
            if P <= 0x03FFFFFF:
                delta_cycles = 3 * (0x03FFFFFF - P + 3)
                cycle_accum += delta_cycles
            elif P == 0x07FFFFFF:
                delta_cycles = 3 * (0x03FFFFFF + 2)
                cycle_accum += delta_cycles
            else:
                item_type = P >> 24
                rem = P & 0x00FFFFFF
                if item_type == 4:
                    ms = rem
                    events.append((cycles_to_us(cycle_accum), 'timestamp_ms', ms))
                elif item_type == 7:
                    if rem & 0x800000:
                        data_id = (rem >> 16) & 0xF
                        length = rem & 0xFFFF
                        # Next `length` bytes (word-aligned) are payload, spread across
                        # subsequent words verbatim (not delta-encoded).
                        payload_words = (length + 3) // 4
                        payload = data[pos:pos + payload_words * 4][:length]
                        pos += payload_words * 4
                        word_count += payload_words
                        if data_id in (0, 1):
                            aux_buf[data_id] += payload
                        events.append((cycles_to_us(cycle_accum), 'aux', (data_id, payload)))
                    elif (rem >> 16) == 1:
                        cpu_hz = (rem & 0xFFFF) * 1_000_000
                        events.append((cycles_to_us(cycle_accum), 'clock_mhz', rem & 0xFFFF))
                    elif (rem >> 16) == 0:
                        fmt_version = rem & 0xFFFF
                        events.append((cycles_to_us(cycle_accum), 'version', fmt_version))
                    else:
                        events.append((cycles_to_us(cycle_accum), 'unknown7', rem))
                else:
                    events.append((cycles_to_us(cycle_accum), 'unknown', P))

        if max_events and len(events) >= max_events:
            break

    # Force-sync flushes pad partial blocks with zero bytes tagged as
    # data_id 0, explicitly documented in rp2350_sniffer.c as
    # "ignored by decoder" -- strip nulls before decoding either channel.
    pin_names = aux_buf[0].replace(b'\x00', b'').decode('ascii', errors='replace').split()
    syslog_text = aux_buf[1].replace(b'\x00', b'').decode('ascii', errors='replace')

    calibrate_events(events)

    return {
        'pin_names': pin_names,
        'cpu_hz': cpu_hz,
        'fmt_version': fmt_version,
        'events': events,
        'syslog_text': syslog_text,
        'word_count': word_count,
        'file_bytes': n,
    }


def calibrate_events(events):
    """Replace each event's raw cycle-accumulated timestamp (which drifts,
    since cpu_hz is only known rounded to the nearest MHz and error
    compounds over millions of cycles) with a value calibrated against the
    firmware's own embedded timestamp_ms markers (true, to the millisecond,
    emitted roughly once per second). Piecewise-linear interpolation
    between the two nearest markers; extrapolates flatly outside the range
    covered by markers. Mutates events in place.
    """
    anchors = [(0.0, 0.0)]  # (raw_us, true_us) -- start of capture is time 0 in both
    for raw_us, kind, payload in events:
        if kind == 'timestamp_ms' and raw_us is not None:
            anchors.append((raw_us, payload * 1000.0))
    if len(anchors) < 2:
        return  # nothing to calibrate against

    anchors.sort()
    raw_pts = [a[0] for a in anchors]
    true_pts = [a[1] for a in anchors]

    import bisect

    def calibrate(raw_us):
        i = bisect.bisect_right(raw_pts, raw_us)
        if i <= 0:
            i = 1
        if i >= len(raw_pts):
            i = len(raw_pts) - 1
        x0, x1 = raw_pts[i - 1], raw_pts[i]
        y0, y1 = true_pts[i - 1], true_pts[i]
        if x1 == x0:
            return y0
        frac = (raw_us - x0) / (x1 - x0)
        return y0 + frac * (y1 - y0)

    for idx in range(len(events)):
        raw_us, kind, payload = events[idx]
        if raw_us is None:
            continue
        events[idx] = (calibrate(raw_us), kind, payload)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else 'zuluscsi_sniff.dat'
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else None
    result = parse(path, max_events=limit)

    print(f"File: {path} ({result['file_bytes']} bytes)")
    print(f"Format version: {result['fmt_version']}")
    print(f"CPU clock: {result['cpu_hz']}")
    print(f"Pin names ({len(result['pin_names'])}): {result['pin_names']}")
    print(f"Total words decoded: {result['word_count']}")
    print(f"Total events: {len(result['events'])}")
    n_pins = sum(1 for e in result['events'] if e[1] == 'pins')
    print(f"Pin-state-change events: {n_pins}")
    print()
    print("--- syslog text (first 4000 chars) ---")
    print(result['syslog_text'][:4000])


if __name__ == '__main__':
    main()
