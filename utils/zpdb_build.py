#!/usr/bin/env python3
"""
zpdb_build.py -- compile ZuluSCSI .ini profiles into a ZPDB flash blob.

ZPDB ("ZuluSCSI Profile DataBase") is a uint32-aligned container that replaces the
statically allocated g_custom_vpd / g_custom_spd / g_custom_modesense tables:
once a section+key is located, the value is a flash offset and a byte length,
read straight into the buffer the response is served from.

The firmware normally builds the store on the device from /zulu_profiles on the
SD card (see src/zpdb_profiles.cpp); this tool produces the same bytes offline,
for inspection and for pre-flashing a ZuluProfiles partition image.

See ZPDB_FORMAT.md for the on-flash layout.

Usage:
    python zpdb_build.py [-o out.bin] [--header out.h] [--report] file.ini [...]
"""

import argparse
import os
import re
import struct
import sys
import time
import zlib

# ---------------------------------------------------------------- constants

ZPDB_MAGIC = 0x42445A50          # 'Z','P','D','B' as a little-endian u32
ZPDB_VERSION = 0x0100            # 1.0
ZPDB_HEADER_SIZE = 32
ZPDB_SECTION_HDR_SIZE = 8
ZPDB_ENTRY_HDR_SIZE = 12
ZPDB_MAX_SECTION = 4096          # one flash page / one assembly buffer
ZPDB_ALIGN = 4

# Longest name the firmware can compare during a lookup (ZPDB_MAX_NAME in
# zpdb.h). A longer name would be stored but never found.
ZPDB_MAX_NAME = 64

TYPE_NULL = 0
TYPE_LONG = 1
TYPE_U64 = 2
TYPE_DATA = 3
TYPE_STR = 4

TYPE_NAMES = {0: "null", 1: "long", 2: "u64", 3: "data", 4: "str"}

# Key schema. Syntax alone cannot decide the type here: "Revision = 1644" and
# "Revision = 5959" are ASCII revision strings, not numbers, while
# "LogSense31PageLength = 0" really is a number. Known keys are typed by name;
# anything unknown falls through to infer_type().
SCHEMA = [
    (re.compile(r"^(Vendor|Product|Revision|Feature|Serial|PartNumber)$", re.I), TYPE_STR),
    (re.compile(r"^Sectors$", re.I), TYPE_U64),
    (re.compile(r"^(BlockSize|.*PageListLength|.*PageLength)$", re.I), TYPE_LONG),
    (re.compile(r"^(SPD|VPD[0-9A-F]{2}|ModeSense[0-9A-F]{2}|LogSense[0-9A-F]{2})$", re.I),
     TYPE_DATA),
]

HEX_TOKENS = re.compile(r"^[0-9A-Fa-f]{2}([\s,]+[0-9A-Fa-f]{2})+$")
INT_RE = re.compile(r"^[+-]?\d+$")

CHUNK_SUFFIX = re.compile(r"^(?P<base>.+)_(?P<idx>\d+)$")
CHUNK_COUNT = re.compile(r"^(?P<base>.+)_chunks$", re.I)


def fnv1a32(text):
    """FNV-1a over the ASCII-uppercased name; matches zpdb_hash() in zpdb.cpp."""
    h = 0x811C9DC5
    for ch in text.upper().encode("ascii", "replace"):
        h = ((h ^ ch) * 0x01000193) & 0xFFFFFFFF
    return h


def align4(n):
    return (n + 3) & ~3


def infer_type(value):
    if value == "":
        return TYPE_NULL
    if HEX_TOKENS.match(value):
        return TYPE_DATA
    if INT_RE.match(value):
        v = int(value)
        return TYPE_LONG if -2 ** 31 <= v < 2 ** 31 else TYPE_U64
    return TYPE_STR


def key_type(key, value):
    for pattern, t in SCHEMA:
        if pattern.match(key):
            # A schema-typed key with an empty value is still a null entry.
            return TYPE_NULL if value == "" else t
    return infer_type(value)


def parse_hex(value):
    out = bytearray()
    for tok in re.split(r"[\s,]+", value.strip()):
        if tok:
            out.append(int(tok, 16))
    return bytes(out)


# ------------------------------------------------------------------ parsing

def parse_ini(path, warn):
    """Return [(section_name, [(key, raw_value), ...]), ...] in file order."""
    sections = []
    current = None
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for lineno, line in enumerate(fh, 1):
            line = line.strip()
            if not line or line[0] in ";#":
                continue
            if line[0] == "[":
                name = line[1:line.index("]")] if "]" in line else line[1:]
                current = (name.strip(), [])
                sections.append(current)
                continue
            if "=" not in line:
                warn("%s:%d: ignoring line without '='" % (path, lineno))
                continue
            key, _, value = line.partition("=")
            key, value = key.strip(), value.strip()
            if current is None:
                warn("%s:%d: key '%s' outside any section, ignored" % (path, lineno, key))
                continue
            current[1].append((key, value))
    return sections


def encode_value(key, t, value, section_name, warn):
    if t == TYPE_NULL:
        return None
    if t == TYPE_DATA:
        return parse_hex(value)
    if t == TYPE_STR:
        return value.encode("ascii", "replace")
    try:
        return int(value, 0)
    except ValueError:
        warn("[%s] %s: '%s' is not numeric, stored as string"
             % (section_name, key, value))
        return value.encode("ascii", "replace")


def collapse(pairs, section_name, warn):
    """Merge Key_0/Key_1/... into Key, drop the redundant Key_chunks counters.

    Returns [(key, type, payload)] where payload is bytes for data/str,
    an int for long/u64, and None for null.
    """
    declared_chunks = {}
    raw = []
    seen = {}

    for key, value in pairs:
        m = CHUNK_COUNT.match(key)
        if m:
            # Redundant with the merged value's byte length; not emitted.
            try:
                declared_chunks[m.group("base").upper()] = int(value)
            except ValueError:
                warn("[%s] %s: non-numeric chunk count '%s'" % (section_name, key, value))
            continue
        if key.upper() in seen:
            warn("[%s] duplicate key '%s', last value wins" % (section_name, key))
            raw[seen[key.upper()]] = (key, value)
            continue
        seen[key.upper()] = len(raw)
        raw.append((key, value))

    # Group the _N chunks, keeping first-appearance order of the base key.
    order = []
    groups = {}
    plain = {}
    for key, value in raw:
        m = CHUNK_SUFFIX.match(key)
        if m:
            base, idx = m.group("base"), int(m.group("idx"))
            ub = base.upper()
            if ub not in groups:
                groups[ub] = (base, {})
                order.append(("chunked", ub))
            if idx in groups[ub][1]:
                warn("[%s] duplicate chunk %s" % (section_name, key))
            groups[ub][1][idx] = value
        else:
            plain[key.upper()] = (key, value)
            order.append(("plain", key.upper()))

    for kind, ub in order:
        if kind == "chunked" and ub in plain:
            warn("[%s] '%s' present both plain and chunked" % (section_name, ub))

    out = []
    for kind, ub in order:
        if kind == "plain":
            key, value = plain[ub]
            t = key_type(key, value)
            out.append((key, t, encode_value(key, t, value, section_name, warn)))
        else:
            base, chunks = groups[ub]
            idxs = sorted(chunks)
            if idxs != list(range(len(idxs))):
                warn("[%s] '%s' chunk indices are not 0..N: %s" % (section_name, base, idxs))
            declared = declared_chunks.get(ub)
            if declared is not None and declared != len(idxs):
                warn("[%s] '%s_chunks' says %d but %d chunks present"
                     % (section_name, base, declared, len(idxs)))
            t = key_type(base, chunks[idxs[0]])
            if t == TYPE_STR:
                payload = "".join(chunks[i] for i in idxs).encode("ascii", "replace")
            else:
                if t != TYPE_DATA:
                    warn("[%s] '%s' is chunked but typed %s; stored as data"
                         % (section_name, base, TYPE_NAMES[t]))
                    t = TYPE_DATA
                payload = b"".join(parse_hex(chunks[i]) for i in idxs)
            out.append((base, t, payload))
    return out


# ---------------------------------------------------------------- emission

def emit_entry(key, t, payload):
    """One 4-aligned entry record. entry_len is back-written once known."""
    kb = key.encode("ascii", "replace")
    if len(kb) >= ZPDB_MAX_NAME:
        raise SystemExit("error: key '%s' is %d bytes; the firmware can only match "
                         "names shorter than %d" % (key, len(kb), ZPDB_MAX_NAME))
    key_field = align4(len(kb) + 1)          # key + NUL, zero padded

    if t == TYPE_NULL:
        val, val_len = b"", 0
    elif t == TYPE_LONG:
        val, val_len = struct.pack("<i", payload), 4
    elif t == TYPE_U64:
        val, val_len = struct.pack("<Q", payload), 8
    else:
        val, val_len = payload, len(payload)

    if t == TYPE_STR:
        # Guarantee at least one terminating NUL so the value doubles as a
        # C string read straight out of flash. val_len excludes it.
        val = val + b"\0" * (align4(val_len + 1) - val_len)
    else:
        val = val + b"\0" * (align4(val_len) - val_len)

    val_off = ZPDB_ENTRY_HDR_SIZE + key_field
    entry_len = val_off + len(val)
    hdr = struct.pack("<HHIHBB", entry_len, val_off, fnv1a32(key),
                      val_len, t, len(kb))
    return hdr + kb + b"\0" * (key_field - len(kb)) + val


def emit_section(name, entries):
    nb = name.encode("ascii", "replace")
    if len(nb) >= ZPDB_MAX_NAME:
        raise SystemExit("error: section name '%s' is %d bytes; the firmware can only "
                         "match names shorter than %d" % (name, len(nb), ZPDB_MAX_NAME))
    name_field = align4(len(nb) + 1)
    body = b"".join(emit_entry(k, t, p) for k, t, p in entries)
    sect_len = ZPDB_SECTION_HDR_SIZE + name_field + len(body)
    if sect_len > ZPDB_MAX_SECTION:
        raise SystemExit("error: section [%s] is %d bytes, over the %d-byte limit"
                         % (name, sect_len, ZPDB_MAX_SECTION))
    if len(entries) > 255:
        raise SystemExit("error: section [%s] has %d entries (max 255)"
                         % (name, len(entries)))
    hdr = struct.pack("<HBBI", sect_len, len(entries), len(nb), fnv1a32(name))
    return hdr + nb + b"\0" * (name_field - len(nb)) + body


def build(paths, warn):
    sections = []
    names = {}
    for path in paths:
        for name, pairs in parse_ini(path, warn):
            if name.upper() in names:
                raise SystemExit("error: duplicate section [%s] in %s (already defined by %s)"
                                 % (name, path, names[name.upper()]))
            names[name.upper()] = path
            sections.append((name, collapse(pairs, name, warn), path))

    blob_sections = b"".join(emit_section(n, e) for n, e, _ in sections)
    total = ZPDB_HEADER_SIZE + len(blob_sections)
    stamp = int(os.environ.get("SOURCE_DATE_EPOCH", time.time()))
    header = struct.pack("<IHHIIIIII", ZPDB_MAGIC, ZPDB_VERSION, ZPDB_HEADER_SIZE,
                         total, len(sections), zlib.crc32(blob_sections) & 0xFFFFFFFF,
                         stamp, 0, 0)
    return header + blob_sections, sections


def section_offsets(blob):
    off = ZPDB_HEADER_SIZE
    count = struct.unpack_from("<I", blob, 12)[0]  # zpdb_header.section_count
    for _ in range(count):
        yield off
        off += struct.unpack_from("<H", blob, off)[0]


# -------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inputs", nargs="+", help=".ini profile files")
    ap.add_argument("-o", "--output", help="output .bin blob")
    ap.add_argument("--header", help="also emit a C header with the blob as an array")
    ap.add_argument("--symbol", default="zpdb_profiles", help="array name for --header")
    ap.add_argument("--report", action="store_true", help="print a size report")
    args = ap.parse_args()

    warnings = []
    blob, sections = build(args.inputs, warnings.append)

    for msg in warnings:
        sys.stderr.write("warning: %s\n" % msg)

    if args.output:
        with open(args.output, "wb") as fh:
            fh.write(blob)

    if args.header:
        with open(args.header, "w") as fh:
            fh.write("// Generated by zpdb_build.py -- do not edit.\n")
            fh.write("#include <stdint.h>\n\n")
            fh.write("const uint32_t %s_size = %d;\n" % (args.symbol, len(blob)))
            fh.write("alignas(4) const uint8_t %s[] = {\n" % args.symbol)
            for i in range(0, len(blob), 16):
                fh.write("    " + " ".join("0x%02x," % b for b in blob[i:i + 16]) + "\n")
            fh.write("};\n")

    if args.report:
        src_bytes = sum(os.path.getsize(p) for p in args.inputs)
        offsets = list(section_offsets(blob))
        print("%-22s %7s %8s  %s" % ("section", "bytes", "entries", "source"))
        print("-" * 70)
        for (name, entries, path), off in zip(sections, offsets):
            sect_len = struct.unpack_from("<H", blob, off)[0]
            print("%-22s %7d %8d  %s" % (name, sect_len, len(entries),
                                         os.path.basename(path)))
        print("-" * 70)
        pages = (len(blob) + 4095) // 4096
        largest = max(struct.unpack_from("<H", blob, o)[0] for o in offsets)
        print("%-22s %7d %8d  (%d files, %d bytes of .ini text)"
              % ("TOTAL", len(blob), sum(len(e) for _, e, _ in sections),
                 len(args.inputs), src_bytes))
        print("largest section: %d bytes (limit %d)" % (largest, ZPDB_MAX_SECTION))
        print("flash footprint: %d x 4096-byte pages (%d bytes)" % (pages, pages * 4096))


if __name__ == "__main__":
    main()
