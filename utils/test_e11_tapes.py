#!/usr/bin/env python3
"""Host-side framing regression tests for ZuluSCSI SIMH/E11 tape images.

Read-only. Run with optional four-tape ZIP:
  python3 utils/test_e11_tapes.py [path/to/tapes.zip]

These verify the firmware's record offset arithmetic, not SCSI hardware timing.
"""
import io
import struct
import sys
import zipfile
from collections import Counter

WORD = struct.Struct("<I")


def at(data, pos):
    assert 0 <= pos <= len(data) - 4, f"truncated word at {pos}"
    return WORD.unpack_from(data, pos)[0]


def stored_length(n, e11):
    return n if e11 else (n + 1) & ~1


def special_e11_tail(data, pos, e11):
    tail = data[pos:]
    return (e11 and 8 <= len(tail) <= 1024 and len(tail) % 4 == 0
            and tail.endswith(b"\x00" * 4)
            and tail[:-4] == b"\xff\xff\x00\x00" * ((len(tail) - 4) // 4))


def scan(data, e11):
    pos, recs, totals = 0, [], Counter()
    while pos < len(data):
        length_class = at(data, pos)
        if length_class == 0:
            recs.append((pos, pos + 4, 0))
            totals["marks"] += 1
            pos += 4
            continue
        if length_class == 0xffffffff or (
                length_class == 0x0000ffff and special_e11_tail(data, pos, e11)):
            totals["terminal_bytes"] = len(data) - pos
            break
        n = length_class & 0x0fffffff
        assert 0 < n <= 2097152, f"invalid/oversized record at {pos}: {n}"
        trailer = pos + 4 + stored_length(n, e11)
        assert trailer + 4 <= len(data), f"truncated record at {pos}"
        assert (at(data, trailer) & 0x0fffffff) == n, (
            f"header/trailer mismatch at {pos} n={n}")
        recs.append((pos, trailer + 4, n))
        totals["data"] += 1
        totals["odd" if n & 1 else "even"] += 1
        pos = trailer + 4

    # Reverse-space from the end of ordinary data, avoiding a special EOM.
    current = pos
    for start, end, n in reversed(recs):
        assert current == end
        if not n:
            assert at(data, current - 4) == 0
            current -= 4
        else:
            assert (at(data, current - 4) & 0x0fffffff) == n
            current -= 8 + stored_length(n, e11)
            assert (at(data, current) & 0x0fffffff) == n
        assert current == start
    assert current == 0
    return totals


def synthetic(lengths, e11):
    out = io.BytesIO()
    for n in lengths:
        out.write(WORD.pack(n))
        out.write(bytes(i % 251 for i in range(n)))
        if n & 1 and not e11:
            out.write(b"\x00")
        out.write(WORD.pack(n))
    out.write(WORD.pack(0))
    return out.getvalue()


def test_synthetic():
    for e11 in (False, True):
        image = synthetic([1, 2, 3, 80, 191, 19261, 32768], e11)
        counts = scan(image, e11)
        assert counts["data"] == 7 and counts["marks"] == 1 and counts["odd"] == 4
        try:
            scan(image, not e11)
        except AssertionError:
            pass
        else:
            raise AssertionError("Wrong framing was accepted")
    image = synthetic([80, 19261], True) + WORD.pack(0xffff) * 60 + WORD.pack(0)
    assert scan(image, True)["terminal_bytes"] == 244
    assert not special_e11_tail(image[:-4] + b"ABCD", len(synthetic([80, 19261], True)), True)
    print("Synthetic SIMH / E11 / reverse SPACE / EOM tests: PASS")


def test_volumes(path):
    totals = Counter()
    with zipfile.ZipFile(path) as archive:
        names = sorted(n for n in archive.namelist() if n.lower().endswith(".tap"))
        assert len(names) == 4, f"expected four tapes, found {len(names)}"
        for name in names:
            image = archive.read(name)  # ZIP CRC is also checked here.
            counts = scan(image, True)
            assert counts["data"] and counts["odd"]
            try:
                scan(image, False)
            except AssertionError:
                pass
            else:
                raise AssertionError(f"Incorrect padded SIMH parsing passed on {name}")
            totals.update(counts)
            print(f"{name}: {dict(counts)} PASS")
    assert totals["data"] == 23344
    assert totals["odd"] == 8161
    assert totals["marks"] == 1467
    print("All four E11 volumes and reverse record positioning: PASS")


if __name__ == "__main__":
    test_synthetic()
    if len(sys.argv) > 1:
        test_volumes(sys.argv[1])
