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
int validate(const unsigned char *bytes, int n)
{
    struct { unsigned char data[256]; int dataLen; } scsiDev = {0};
    if (n < 4 || n > 256) return 0;
    memcpy(scsiDev.data, bytes, n);
    scsiDev.dataLen = n;
    int idx = 4;
    int blockDescLen = scsiDev.data[3];
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
    lib.validate.argtypes = [ctypes.POINTER(ctypes.c_ubyte), ctypes.c_int]
    lib.validate.restype = ctypes.c_int
    return lib.validate


def assert_valid(fn, data, expected, why):
    arr = (ctypes.c_ubyte * len(data)).from_buffer_copy(data)
    observed = bool(fn(arr, len(data)))
    if observed != expected:
        raise AssertionError(f"{why}: expected {expected}, got {observed} ({data.hex(' ')})")


def main():
    with tempfile.TemporaryDirectory(prefix="as400-mode-select-") as tmp:
        validator = compiled_validator(tmp)
        tests = [
            (B10, True, "real B10 IBM 0671S15 22-byte request"),
            (B10[:3], False, "truncated mode header"),
            (B10[:5], False, "block descriptor exceeds provided bytes"),
            (B10[:13] + b"\x01", False, "truncated normal page header"),
            (B10[:15] + b"\x02\x0e\x01\x80", False, "oversized second page"),
            (bytes.fromhex("00 00 00 00 01 01 04 02 02 01 80"), True,
             "two normal pages with different lengths"),
            (bytes.fromhex("00 00 00 00 00"), True,
             "vendor page code zero ends parsing as original firmware does"),
        ]
        for data, expected, why in tests:
            assert_valid(validator, data, expected, why)
        print(f"PASS: {len(tests)} MODE SELECT bounds regression cases")


if __name__ == "__main__":
    main()
