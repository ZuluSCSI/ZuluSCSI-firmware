#!/usr/bin/env python3
"""
zpdb_dump.py -- read back a ZPDB blob and, optionally, prove it round-trips.

The walk here mirrors what zpdb.cpp does: skip a section by its own
`sect_len`, skip an entry by its own `entry_len`, take a value as
(pointer, val_len). It exists to check the format and the builder against
each other without hardware in the loop.

Usage:
    python zpdb_dump.py store.bin                     # structural dump
    python zpdb_dump.py store.bin --verify a.ini ...  # round-trip check
"""

import argparse
import struct
import sys
import zlib

import zpdb_build as zb


class ZpdbError(Exception):
    pass


class Entry(object):
    __slots__ = ("key", "type", "value", "offset", "entry_len")

    def __init__(self, key, type_, value, offset, entry_len):
        self.key = key
        self.type = type_
        self.value = value
        self.offset = offset
        self.entry_len = entry_len


def read_cstr(blob, off, length):
    name = blob[off:off + length].decode("ascii")
    if blob[off + length] != 0:
        raise ZpdbError("name at 0x%x is not NUL-terminated" % off)
    return name


def parse_entry(blob, off, limit):
    if off + zb.ZPDB_ENTRY_HDR_SIZE > limit:
        raise ZpdbError("entry header at 0x%x runs past its section" % off)

    entry_len, val_off, key_hash, val_len, type_, key_len = struct.unpack_from(
        "<HHIHBB", blob, off)

    if entry_len % 4 or entry_len < zb.ZPDB_ENTRY_HDR_SIZE:
        raise ZpdbError("entry at 0x%x has a bad length %d" % (off, entry_len))
    if off + entry_len > limit:
        raise ZpdbError("entry at 0x%x overruns its section" % off)
    if (off + val_off) % 4:
        raise ZpdbError("value at 0x%x is not uint32-aligned" % (off + val_off))
    if val_off + val_len > entry_len:
        raise ZpdbError("value at 0x%x overruns its entry" % (off + val_off))

    key = read_cstr(blob, off + zb.ZPDB_ENTRY_HDR_SIZE, key_len)
    if zb.fnv1a32(key) != key_hash:
        raise ZpdbError("key '%s' hash mismatch" % key)
    if val_off != zb.ZPDB_ENTRY_HDR_SIZE + zb.align4(key_len + 1):
        raise ZpdbError("key '%s' has an inconsistent val_off" % key)

    raw = blob[off + val_off:off + val_off + val_len]

    if type_ == zb.TYPE_NULL:
        if val_len:
            raise ZpdbError("null entry '%s' has a %d-byte value" % (key, val_len))
        value = None
    elif type_ == zb.TYPE_LONG:
        value = struct.unpack("<i", raw)[0]
    elif type_ == zb.TYPE_U64:
        value = struct.unpack("<Q", raw)[0]
    elif type_ == zb.TYPE_DATA:
        value = bytes(raw)
    elif type_ == zb.TYPE_STR:
        if blob[off + val_off + val_len] != 0:
            raise ZpdbError("string '%s' is not NUL-terminated" % key)
        value = raw.decode("ascii")
    else:
        raise ZpdbError("entry '%s' has unknown type %d" % (key, type_))

    return Entry(key, type_, value, off, entry_len)


def parse(blob):
    if len(blob) < zb.ZPDB_HEADER_SIZE:
        raise ZpdbError("shorter than a header")

    (magic, version, header_size, total_size, section_count,
     crc, epoch, flags, reserved) = struct.unpack_from("<IHHIIIIII", blob, 0)

    if magic != zb.ZPDB_MAGIC:
        raise ZpdbError("bad magic 0x%08x" % magic)
    if header_size != zb.ZPDB_HEADER_SIZE:
        raise ZpdbError("unexpected header_size %d" % header_size)
    if total_size != len(blob):
        raise ZpdbError("total_size %d but the file holds %d bytes"
                        % (total_size, len(blob)))
    if total_size % 4:
        raise ZpdbError("total_size %d is not uint32-aligned" % total_size)

    want = zlib.crc32(blob[header_size:total_size]) & 0xFFFFFFFF
    if want != crc:
        raise ZpdbError("CRC mismatch: computed 0x%08x, stored 0x%08x" % (want, crc))

    sections = []
    off = header_size
    for _ in range(section_count):
        sect_len, entry_count, name_len, name_hash = struct.unpack_from("<HBBI", blob, off)

        if sect_len % 4 or sect_len < zb.ZPDB_SECTION_HDR_SIZE:
            raise ZpdbError("section at 0x%x has a bad length %d" % (off, sect_len))
        if sect_len > zb.ZPDB_MAX_SECTION:
            raise ZpdbError("section at 0x%x is %d bytes, over the page limit"
                            % (off, sect_len))
        if off + sect_len > total_size:
            raise ZpdbError("section at 0x%x overruns the store" % off)
        if off % 4:
            raise ZpdbError("section at 0x%x is not uint32-aligned" % off)

        name = read_cstr(blob, off + zb.ZPDB_SECTION_HDR_SIZE, name_len)
        if zb.fnv1a32(name) != name_hash:
            raise ZpdbError("section '%s' hash mismatch" % name)

        entries = []
        eoff = off + zb.ZPDB_SECTION_HDR_SIZE + zb.align4(name_len + 1)
        for _ in range(entry_count):
            e = parse_entry(blob, eoff, off + sect_len)
            entries.append(e)
            eoff += e.entry_len

        # The entries must fill the section exactly -- otherwise sect_len is
        # not a trustworthy skip and the scan would drift.
        if eoff != off + sect_len:
            raise ZpdbError("section '%s' has %d bytes unaccounted for"
                            % (name, off + sect_len - eoff))

        sections.append((name, sect_len, off, entries))
        off += sect_len

    if off != total_size:
        raise ZpdbError("%d trailing bytes after the last section" % (total_size - off))

    return {"version": version, "epoch": epoch, "flags": flags,
            "reserved": reserved, "sections": sections}


def fmt(entry):
    if entry.type == zb.TYPE_DATA:
        head = " ".join("%02x" % b for b in entry.value[:12])
        tail = " ..." if len(entry.value) > 12 else ""
        return "%d bytes: %s%s" % (len(entry.value), head, tail)
    if entry.type == zb.TYPE_STR:
        return "'%s'" % entry.value
    if entry.type == zb.TYPE_NULL:
        return "(null)"
    return str(entry.value)


def verify(store, ini_paths):
    """Re-parse the sources and confirm nothing was lost or changed."""
    warnings = []
    expected = {}
    for path in ini_paths:
        for name, pairs in zb.parse_ini(path, warnings.append):
            expected[name] = zb.collapse(pairs, name, warnings.append)

    got = {name: entries for name, _, _, entries in store["sections"]}
    problems = []

    for name in expected:
        if name not in got:
            problems.append("section [%s] is missing from the store" % name)

    for name in got:
        if name not in expected:
            problems.append("section [%s] is in the store but not the sources" % name)

    for name, want in expected.items():
        have = {e.key: e for e in got.get(name, [])}
        if len(have) != len(got.get(name, [])):
            problems.append("[%s] has duplicate keys in the store" % name)

        for key, type_, payload in want:
            e = have.pop(key, None)
            if e is None:
                problems.append("[%s] %s is missing" % (name, key))
                continue
            if e.type != type_:
                problems.append("[%s] %s: type %s, expected %s"
                                % (name, key, zb.TYPE_NAMES[e.type], zb.TYPE_NAMES[type_]))
                continue
            if type_ == zb.TYPE_STR:
                want_val = payload.decode("ascii")
            elif type_ == zb.TYPE_DATA:
                want_val = payload
            else:
                want_val = payload
            if e.value != want_val:
                problems.append("[%s] %s: value differs" % (name, key))

        for key in have:
            problems.append("[%s] %s is in the store but not the sources" % (name, key))

    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("blob")
    ap.add_argument("--verify", nargs="*", metavar="INI",
                    help="re-parse these sources and diff them against the store")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    with open(args.blob, "rb") as fh:
        blob = fh.read()

    try:
        store = parse(blob)
    except ZpdbError as exc:
        sys.stderr.write("invalid store: %s\n" % exc)
        return 1

    if not args.quiet:
        print("ZPDB v%d.%d, %d bytes, %d sections, crc ok"
              % (store["version"] >> 8, store["version"] & 0xFF,
                 len(blob), len(store["sections"])))
        for name, sect_len, off, entries in store["sections"]:
            print("\n[%s]  @0x%05x  %d bytes  %d entries" % (name, off, sect_len, len(entries)))
            for e in entries:
                print("    %-16s %-5s %s" % (e.key, zb.TYPE_NAMES[e.type], fmt(e)))

    if args.verify is not None:
        problems = verify(store, args.verify)
        if problems:
            for p in problems:
                sys.stderr.write("MISMATCH: %s\n" % p)
            return 1
        total = sum(len(e) for _, _, _, e in store["sections"])
        print("\nround-trip ok: %d sections, %d entries match their sources"
              % (len(store["sections"]), total))

    return 0


if __name__ == "__main__":
    sys.exit(main())
