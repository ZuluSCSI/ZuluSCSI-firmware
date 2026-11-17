#!/usr/bin/env python3
"""Compile and test the real AS400_DiskSerialNumber normalization from
custom_vendor_inquiry.cpp.

The standard INQUIRY serves the first serial character as '0', so a value is
only accepted when it can be served unchanged after zero-padding.
"""
import ctypes
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/custom_vendor_inquiry.cpp"


def compiled_normalizer(workdir):
    src = SOURCE.read_text()
    start = src.index("static bool normalizeAS400Serial(")
    stop = src.index("\n}\n", start) + 3
    fragment = src[start:stop]
    c_code = r"""
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
""" + fragment + r"""
int normalize(const char *value, char *out)
{
    return normalizeAS400Serial(value, out) ? 1 : 0;
}
"""
    c_path = Path(workdir) / "serial.c"
    so_path = Path(workdir) / "serial.so"
    c_path.write_text(c_code)
    subprocess.run(
        [os.environ.get("CC", "cc"), "-std=c99", "-Wall", "-Wextra",
         "-Werror", "-fPIC", "-shared", str(c_path), "-o", str(so_path)],
        check=True,
    )
    lib = ctypes.CDLL(str(so_path))
    lib.normalize.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
    lib.normalize.restype = ctypes.c_int
    return lib.normalize


def main():
    with tempfile.TemporaryDirectory(prefix="as400-serial-") as tmp:
        normalize = compiled_normalizer(tmp)
        tests = [
            ("01111111", "01111111", "README example, 8 digits"),
            ("02222222", "02222222", "README example, 8 digits"),
            ("00044872", "00044872", "captured 53P3239 serial"),
            ("214A872", "0214A872", "zuluscsi.ini example, 7 digits"),
            ("100001", "00100001", "6 digits keep their leading 1"),
            ("1", "00000001", "single digit"),
            ("0abcdef1", "0ABCDEF1", "lowercase hex is uppercased"),
            ("10000001", None, "8 digits not starting with 0"),
            ("20000001", None, "8 digits not starting with 0"),
            ("012345678", None, "longer than 8 characters"),
            ("0123456G", None, "non-hex character"),
            ("01 23456", None, "embedded space"),
            ("", None, "empty value"),
        ]
        for value, expected, why in tests:
            out = ctypes.create_string_buffer(9)
            ok = bool(normalize(value.encode(), out))
            observed = out.value.decode() if ok else None
            if observed != expected:
                raise AssertionError(f"{why}: {value!r} -> {observed!r}, expected {expected!r}")
        print(f"PASS: {len(tests)} AS400_DiskSerialNumber normalization cases")


if __name__ == "__main__":
    main()
