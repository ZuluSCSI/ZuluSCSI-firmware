#!/usr/bin/env python3
"""
Decode a ZuluSCSI RP2350 bus sniffer raw capture (zuluscsi_sniff.dat) directly
into a readable SCSI phase/command trace, without needing sigrok/PulseView.

Builds on decode_raw_sniff.py's raw event parser and reimplements the same
phase-decode state machine as utils/sigrok_scsi_decoder/pd.py (the sigrok
plugin), adapted for two hardware quirks specific to boards whose sniffer
pin list combines signals onto shared physical GPIOs (confirmed for
ZuluSCSI Blaster in ZuluSCSI_platform_gpio_Blaster.h: SCSI_IN_SEL/SCSI_IN_CD
both GPIO23, SCSI_IN_BSY/SCSI_IN_MSG both GPIO26 -- the firmware itself
relies on the same context-dependent handoff this decoder reimplements: SEL
and BSY are only meaningful before a connection is established; CD and MSG
take over the same physical nets once the target has taken control after
selection). If a capture's pin list has separate BSY/MSG/CD/SEL entries
(e.g. a board without this sharing), the decoder uses them directly instead.

Usage:
    python3 decode_scsi_trace.py <zuluscsi_sniff.dat> [--start SEC] [--end SEC] [--limit N]
"""
import sys
import argparse
from decode_raw_sniff import parse

# Category: 0 = control, 1 = read, 2 = write, 3 = special
# (same table as pd.py, kept in sync manually)
SCSI_COMMANDS = {
    0x00: (0, "TestUnitReady"), 0x01: (0, "RezeroUnit/Rewind"), 0x03: (0, "RequestSense"),
    0x04: (0, "FormatUnit"), 0x05: (0, "ReadBlockLimits"), 0x06: (3, "IomegaVendorCommand"),
    0x08: (1, "Read6"), 0x0A: (2, "Write6"), 0x0B: (1, "Seek6"),
    0x0C: (3, "Xebec InitializeDriveCharacteristics"), 0x0F: (3, "Xebec WriteSectorBuffer"),
    0x10: (3, "WriteFilemarks"), 0x11: (1, "Space"), 0x12: (0, "Inquiry"), 0x13: (1, "Verify"),
    0x15: (0, "ModeSelect6"), 0x16: (2, "Reserve"), 0x17: (2, "Release"), 0x19: (2, "Erase"),
    0x1A: (0, "ModeSense"), 0x1B: (0, "StartStopUnit"), 0x1C: (0, "ReceiveDiagnostic"),
    0x1D: (0, "SendDiagnostic"), 0x1E: (0, "PreventAllowMediumRemoval"), 0x25: (0, "ReadCapacity"),
    0x28: (1, "Read10"), 0x2A: (2, "Write10"), 0x2B: (1, "Seek10"), 0x2C: (2, "Erase10"),
    0x2E: (2, "WriteVerify"), 0x2F: (1, "Verify"), 0x34: (1, "PreFetch/ReadPosition"),
    0x35: (0, "SynchronizeCache"), 0x36: (0, "LockUnlockCache"), 0x37: (0, "ReadDefectData"),
    0x3B: (2, "WriteBuffer"), 0x3C: (1, "ReadBuffer"), 0x55: (0, "ModeSelect10"),
    0x5A: (0, "ModeSense10"), 0x88: (1, "Read16"), 0x8A: (2, "Write16"),
}

STATE_NAMES = {
    -1: "BUS_FREE", -2: "BUS_BUSY", -3: "ARBITRATION", -4: "SELECTION",
    6: "STATUS", 2: "COMMAND", 4: "DATA_IN", 0: "DATA_OUT", 7: "MESSAGE_IN", 3: "MESSAGE_OUT",
}
BIT_IO, BIT_CD, BIT_MSG = 4, 2, 1


def build_pin_index(pin_names):
    idx = {name: i for i, name in enumerate(pin_names)}
    return idx


def bit(pinstate, i):
    return (pinstate >> i) & 1


def decode(path=None, start_s=None, end_s=None, limit=None, result=None):
    if result is None:
        result = parse(path)
    pin_names = result['pin_names']
    idx = build_pin_index(pin_names)
    print(f"# Pin index: {idx}", file=sys.stderr)

    # Signals that are always independently sensed
    i_req = idx['REQ']
    i_ack = idx['ACK']
    i_io = idx['IO']
    i_atn = idx.get('ATN')
    i_datadir = idx.get('DATA_DIR')
    i_dbp = idx.get('DBP')
    db_indices = [idx[f'DB{n}'] for n in range(8) if f'DB{n}' in idx]

    # Shared-pin handling: CD_SEL / MSG_BSY on Blaster; separate CD/SEL/MSG/BSY
    # on boards without this sharing.
    shared_cdsel = 'CD_SEL' in idx
    shared_msgbsy = 'MSG_BSY' in idx
    i_cdsel = idx.get('CD_SEL', idx.get('CD'))
    i_sel = idx.get('CD_SEL', idx.get('SEL'))
    i_msgbsy = idx.get('MSG_BSY', idx.get('MSG'))
    i_bsy = idx.get('MSG_BSY', idx.get('BSY'))

    state = -1  # BUS_FREE
    connected = False  # True once selection has completed and CD_SEL/MSG_BSY mean CD/MSG
    cmd_bytes = []
    phase_data = []
    pending_data = None
    prev_req = None
    prev_ack = None
    prev_pins = None
    ss_state_time = None
    idle_streak = 0
    lines = []

    def cur_cd(pins):
        return bit(pins, i_cdsel)  # meaningful as CD only once connected

    def cur_msg(pins):
        return bit(pins, i_msgbsy)  # meaningful as MSG only once connected

    def cur_bsy(pins):
        return bit(pins, i_bsy)  # meaningful as BSY only while not connected

    def db_ids(pins):
        # During SELECTION the initiator asserts both its own ID bit and the
        # target ID bit on the data bus (OR'd together, active low).
        mask = 0
        for i, dbi in enumerate(db_indices):
            if bit(pins, dbi) == 0:
                mask |= (1 << i)
        return mask, [i for i in range(8) if mask & (1 << i)]

    def push_cmd(t):
        if cmd_bytes:
            cls, name = SCSI_COMMANDS.get(cmd_bytes[0], (3, "UNKNOWN"))
            lines.append(f"[{t/1000:.3f}ms] COMMAND: 0x{cmd_bytes[0]:02x} ({name}) bytes={' '.join('%02x'%b for b in cmd_bytes)}")

    def push_state(t):
        n = STATE_NAMES.get(state, str(state))
        if phase_data:
            preview = ' '.join('%02x' % d for d in phase_data[:16])
            more = '...' if len(phase_data) > 16 else ''
            lines.append(f"[{t/1000:.3f}ms] {n} ({len(phase_data)} bytes: {preview}{more})")
        else:
            lines.append(f"[{t/1000:.3f}ms] {n}")

    n_events = 0
    for us, kind, payload in result['events']:
        if kind != 'pins':
            continue
        if start_s is not None and us < start_s * 1e6:
            continue
        if end_s is not None and us > end_s * 1e6:
            break
        n_events += 1
        if limit and n_events > limit:
            break

        pins = payload
        req = bit(pins, i_req)
        ack = bit(pins, i_ack)

        if prev_pins is None:
            prev_pins = pins
            prev_req = req
            prev_ack = ack
            continue

        if not connected:
            # Watching MSG_BSY (as BSY) for start of a new transaction
            b = cur_bsy(pins)
            prev_b = cur_bsy(prev_pins)
            if b == 0 and prev_b == 1 and state == -1:
                state = -2  # BUS_BUSY
                ss_state_time = us
                cmd_bytes = []
                phase_data = []
                pending_data = None
                mask, bits = db_ids(pins)
                lines.append(f"[{us/1000:.3f}ms] ---- SELECTION (BSY asserted) IDs on bus: {bits} (mask 0x{mask:02x})")
            elif b == 1 and prev_b == 0 and state != -1 and not connected:
                # BSY released again before a real connection formed (glitch,
                # arbitration loss, or a bus-free we didn't otherwise detect)
                if state != -2:
                    push_state(us)
                state = -1
                connected = False

        # Detect the moment REQ is first asserted after BSY went busy --
        # that's the target taking control, at which point CD_SEL/MSG_BSY
        # switch meaning to CD/MSG for the rest of the connection.
        if not connected and state == -2 and req == 0 and prev_req == 1:
            connected = True
            state = 2 if cur_cd(pins) else 0  # best guess, refined below on REQ falling edges
            ss_state_time = us
            lines.append(f"[{us/1000:.3f}ms] ---- connection established (REQ asserted)")

        if connected:
            new_state = 0
            if cur_msg(pins) == 0: new_state |= BIT_MSG
            if cur_cd(pins) == 0: new_state |= BIT_CD
            if bit(pins, i_io) == 0: new_state |= BIT_IO

            if req == 0 and prev_req == 1:
                if new_state != state:
                    push_state(us)
                    push_cmd(us) if state == 2 else None
                    state = new_state
                    ss_state_time = us
                    phase_data = []
                    pending_data = None

            # Data latch: to initiator on REQ falling edge, to target on ACK falling edge
            if state & BIT_IO:
                latch = (req == 0 and prev_req == 1)
            else:
                latch = (ack == 0 and prev_ack == 1)

            if latch:
                dbyte = 0
                for i, dbi in enumerate(db_indices):
                    if bit(pins, dbi) == 0:  # active low
                        dbyte |= (1 << i)
                phase_data.append(dbyte)
                if state == 2:
                    cmd_bytes.append(dbyte)

            # Bus-free detection: CD_SEL/MSG_BSY only mean CD/MSG while
            # connected, so we have no dedicated BSY line to watch for
            # release. Instead: every real phase asserts at least one of
            # CD/MSG/IO (new_state != 0) or, for DATA_OUT (new_state == 0),
            # shows REQ/ACK handshaking. A genuine disconnect shows ALL
            # control lines idle (CD/MSG/IO/REQ/ACK all released) with no
            # handshaking -- require it sustained across 2 consecutive
            # samples to reject single-sample transition glitches.
            all_idle = (cur_msg(pins) == 1 and cur_cd(pins) == 1 and
                        bit(pins, i_io) == 1 and req == 1 and ack == 1)
            if all_idle:
                idle_streak += 1
                if idle_streak >= 2 and connected:
                    push_cmd(us) if state == 2 else None
                    push_state(us)
                    lines.append(f"[{us/1000:.3f}ms] ---- BUS FREE (disconnect)")
                    connected = False
                    state = -1
                    cmd_bytes = []
                    phase_data = []
                    pending_data = None
            else:
                idle_streak = 0

        prev_pins = pins
        prev_req = req
        prev_ack = ack

    if state != -1:
        push_cmd(us)
        push_state(us)

    return lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('path')
    ap.add_argument('--start', type=float, default=None, help='start time in seconds')
    ap.add_argument('--end', type=float, default=None, help='end time in seconds')
    ap.add_argument('--limit', type=int, default=None, help='max pin events to process')
    args = ap.parse_args()

    lines = decode(args.path, args.start, args.end, args.limit)
    for l in lines:
        print(l)


if __name__ == '__main__':
    main()
