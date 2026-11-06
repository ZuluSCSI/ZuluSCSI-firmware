#!/usr/bin/env python3
"""Compile and test the real MODE SELECT prevalidation fragment from mode.c.

This focuses on descriptor/page bounds; it does not simulate an entire SCSI
command or the AS/400's later short-page personality check.
"""
import ctypes
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "lib/SCSI2SD/src/firmware/mode.c"

# Actual MODE SELECT(6) parameter bytes captured from IBM 9404-B10 -> 0671S15.
B10 = bytes.fromhex(
    "00 00 00 08 "
    "00 09 67 C8 00 00 02 08 "
    "01 01 04 "
    "02 02 01 80 "
    "00 01 80"
)


def compiled_validator(workdir):
    src = SOURCE.read_text()
    start_marker = "// Validate the whole parameter list before changing live settings."
    end_marker = "// Store device-specific parameter byte"
    start = src.index(start_marker, src.index("static void doModeSelect(void)"))
    stop = src.index(end_marker, start)
    fragment = src[start:stop]
    c_code = r"""
#include <stdint.h>
#include <string.h>
int validate(const unsigned char *bytes, int n, int ten_byte)
{
    struct { unsigned char data[256]; int dataLen; } scsiDev = {0};
    if (n < (ten_byte ? 8 : 4) || n > 256) return 0;
    memcpy(scsiDev.data, bytes, n);
    scsiDev.dataLen = n;
    int idx = ten_byte ? 8 : 4;
    int blockDescLen = ten_byte ? (scsiDev.data[6] << 8) | scsiDev.data[7]
                                : scsiDev.data[3];
""" + fragment + r"""
    return 1;
badLength:
    return 0;
}
"""
    c_path = Path(workdir) / "validator.c"
    so_path = Path(workdir) / "validator.so"
    c_path.write_text(c_code)
    subprocess.run(
        [os.environ.get("CC", "cc"), "-std=c99", "-Wall", "-Wextra",
         "-Werror", "-fPIC", "-shared", str(c_path), "-o", str(so_path)],
        check=True,
    )
    lib = ctypes.CDLL(str(so_path))
    lib.validate.argtypes = [ctypes.POINTER(ctypes.c_ubyte), ctypes.c_int, ctypes.c_int]
    lib.validate.restype = ctypes.c_int
    return lib.validate


def assert_valid(fn, data, ten_byte, expected, why):
    arr = (ctypes.c_ubyte * len(data)).from_buffer_copy(data)
    observed = bool(fn(arr, len(data), int(ten_byte)))
    if observed != expected:
        raise AssertionError(f"{why}: expected {expected}, got {observed} ({data.hex(' ')})")


def main():
    with tempfile.TemporaryDirectory(prefix="as400-mode-select-") as tmp:
        validator = compiled_validator(tmp)
        # MODE SELECT(10) uses an 8-byte header and a 16-bit descriptor
        # length; the three page payloads and IBM descriptor are unchanged.
        b10_ten = bytes.fromhex("00 00 00 00 00 00 00 08") + B10[4:]
        tests = [
            (B10, False, True, "real B10 IBM 0671S15 MODE SELECT(6)"),
            (B10[:3], False, False, "truncated 6-byte command header"),
            (B10[:5], False, False, "block descriptor exceeds available bytes"),
            (B10[:13] + b"\x01", False, False, "truncated normal mode page"),
            (B10[:15] + b"\x02\x0e\x01\x80", False, False,
             "oversized second page"),
            (bytes.fromhex("00 00 00 00 01 01 04 02 02 01 80"),
             False, True, "two normal pages of different sizes"),
            (bytes.fromhex("00 00 00 00 00"), False, True,
             "page-code zero ends parsing as existing firmware does"),
            (b10_ten, True, True, "MODE SELECT(10) with matching pages"),
            (b10_ten[:7], True, False, "truncated 10-byte command header"),
            (b10_ten[:12], True, False, "truncated 10-byte block descriptor"),
            (b10_ten[:17] + b"\x01", True, False, "truncated 10-byte mode page"),
            (b10_ten[:19] + b"\x02\x0e\x01\x80", True, False,
             "oversized second page in MODE SELECT(10)"),
            (b10_ten[:16], True, True, "MODE SELECT(10) descriptor-only"),
        ]
        for data, ten_byte, expected, why in tests:
            assert_valid(validator, data, ten_byte, expected, why)
        print(f"PASS: {len(tests)} MODE SELECT bounds regression cases")


if __name__ == "__main__":
    main()
