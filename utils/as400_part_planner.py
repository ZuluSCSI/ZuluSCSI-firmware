#!/usr/bin/env python3
"""Computes an exact, zero-waste GPT partition layout for an SD card holding
one FAT32/exFAT admin partition (zuluscsi.ini, firmware, tape/CD images)
plus one PART:n partition per AS/400 disk profile, sized to exactly what
AlignUnalignedAccesses needs (not "however much felt safe") and aligned to
the SD card's own preferred AU_SIZE boundary.

Ports the CISC/PPC gapped-size formulas from src/ZuluSCSI_gap_layout.h/.cpp
by hand (Python, unlike this project's other host-side utilities, has no
practical way to compile against that C++ source directly) -- the same
class of duplication utils/as400_gapconv.c already accepts for the same
underlying math. GAP_CISC_SLOT_SIZE/GAP_PPC_GROUP_SECTORS/GAP_PPC_GROUP_PHYS_SIZE
below MUST stay in exact lockstep with those two files' own copies of the
same constants.

Never drives gdisk itself -- prints a plain table of exact start/end
sectors, a human types them in.
"""

import argparse
import sys

# Must match src/ZuluSCSI_gap_layout.h and utils/as400_gapconv.c exactly.
GAP_CISC_SLOT_SIZE = 1024
GAP_PPC_GROUP_SECTORS = 8
GAP_PPC_GROUP_PHYS_SIZE = 9 * 512

GPT_ZULUSCSI_PART_TYPE_GUID = "E5BF00BF-E1B8-4E16-945C-5AB326258BCC"

# Standard 128-entry GPT reserved areas: front = 1 protective MBR + 1 primary
# GPT header + 32-sector partition array = 34 sectors; back = 32-sector
# backup array + 1 backup GPT header = 33 sectors. Matches gdisk's own
# defaults.
GPT_FRONT_RESERVED = 34
GPT_BACK_RESERVED = 33


def gap_resolve_scheme(block_size):
    """520->'cisc', 522->'ppc', anything else->None (no gapped scheme)."""
    if block_size == 520:
        return "cisc"
    elif block_size == 522:
        return "ppc"
    else:
        return None


def gap_physical_size(scheme, sectors):
    """Physical (gapped) byte size needed for `sectors` logical sectors."""
    if scheme == "cisc":
        return sectors * GAP_CISC_SLOT_SIZE
    elif scheme == "ppc":
        groups = (sectors + GAP_PPC_GROUP_SECTORS - 1) // GAP_PPC_GROUP_SECTORS
        return groups * GAP_PPC_GROUP_PHYS_SIZE
    else:
        raise ValueError(f"unknown scheme {scheme!r}")


def lookup_profile(definitions_path, profile_name):
    """Reads BlockSize=/Sectors= from within [profile_name]'s own section of
    as400_disk_definitions.txt -- ignores every other key and comment, and
    stops at the section's end (the next '[' line)."""
    section_target = f"[{profile_name}]".lower()
    in_section = False
    block_size = None
    sectors = None

    try:
        with open(definitions_path, "r", errors="replace") as f:
            for raw_line in f:
                line = raw_line.strip()

                if line.startswith("["):
                    if in_section:
                        break  # left our section, no need to read further
                    in_section = (line.lower() == section_target)
                    continue

                if not in_section or not line or line[0] in (";", "#"):
                    continue

                if "=" not in line:
                    continue
                key, _, value = line.partition("=")
                key = key.strip().lower()
                value = value.strip()

                if key == "blocksize":
                    block_size = int(value, 0)
                elif key == "sectors":
                    sectors = int(value, 0)
    except OSError as e:
        print(f"Error: cannot open {definitions_path}: {e}", file=sys.stderr)
        return None

    if block_size is None or sectors is None:
        print(f"Error: profile '{profile_name}' not found (or missing BlockSize/Sectors) "
              f"in {definitions_path}", file=sys.stderr)
        return None

    return {"name": profile_name, "block_size": block_size, "sectors": sectors}


def main():
    parser = argparse.ArgumentParser(
        description="Compute an exact, zero-waste GPT partition layout for an AS/400 SD card.",
        epilog="Remaining space after all requested profiles are sized (each rounded up to a "
               "whole AU_SIZE unit, so every partition starts AU-aligned) becomes the PART:1 "
               "FAT32/exFAT admin partition -- no unallocated space left anywhere.")
    parser.add_argument("--total-sectors", type=int, required=True,
                         help="the SD card's total sector count "
                              "(gdisk -l /dev/sdX prints this as \"Disk /dev/sdX: N sectors\")")
    parser.add_argument("--au-size-sectors", type=int, required=True,
                         help="the card's preferred alignment, in 512-byte sectors -- read "
                              "directly off the Zulu console's own boot log line "
                              "\"SD preferred alignment: N sectors (...)\"")
    parser.add_argument("--profile", action="append", dest="profiles", default=[], metavar="NAME",
                         help="an AS/400 disk profile to place, by its as400_disk_definitions.txt "
                              "section name. Repeat in the order you want them assigned "
                              "PART:2, PART:3, ...")
    parser.add_argument("--definitions", default="as400_disk_definitions.txt", metavar="PATH",
                         help="path to as400_disk_definitions.txt (default: %(default)s)")
    args = parser.parse_args()

    if not args.profiles:
        parser.error("at least one --profile=NAME is required")

    profiles = []
    for name in args.profiles:
        info = lookup_profile(args.definitions, name)
        if info is None:
            return 1
        profiles.append(info)

    first_usable = ((GPT_FRONT_RESERVED + args.au_size_sectors - 1) // args.au_size_sectors) * args.au_size_sectors

    if args.total_sectors <= GPT_BACK_RESERVED:
        print(f"Error: --total-sectors={args.total_sectors} is too small", file=sys.stderr)
        return 1
    last_usable = args.total_sectors - 1 - GPT_BACK_RESERVED

    if last_usable <= first_usable:
        print("Error: disk too small to fit the GPT reserved areas plus one AU-aligned partition",
              file=sys.stderr)
        return 1

    usable_sectors = last_usable - first_usable + 1
    total_aus = usable_sectors // args.au_size_sectors

    as400_allocated_sectors = []
    as400_total_aus = 0
    for p in profiles:
        scheme = gap_resolve_scheme(p["block_size"])
        if scheme is None:
            print(f"Error: profile '{p['name']}' has block size {p['block_size']}, "
                  f"not a recognized AS/400 scheme (520 or 522)", file=sys.stderr)
            return 1

        needed_bytes = gap_physical_size(scheme, p["sectors"])
        needed_sectors = needed_bytes // 512
        allocated_aus = (needed_sectors + args.au_size_sectors - 1) // args.au_size_sectors
        as400_allocated_sectors.append(allocated_aus * args.au_size_sectors)
        as400_total_aus += allocated_aus

    if as400_total_aus >= total_aus:
        print(f"Error: requested profiles need {as400_total_aus} AU units, but this disk only "
              f"has {total_aus} available (nothing would be left for the FAT32/exFAT partition)",
              file=sys.stderr)
        return 1

    fat_aus = total_aus - as400_total_aus
    fat_sectors = fat_aus * args.au_size_sectors
    # Sub-AU remainder from the integer division above (always < au_size_sectors,
    # typically a few MB at most) -- absorbed into the last AS/400 partition's
    # tail rather than left unallocated. Safe: an AS/400 partition larger than
    # its profile strictly needs is harmless (ZuluSCSI_disk.cpp clamps the
    # reported SCSI capacity to the profile's own declared sector count
    # regardless of how much physical space the partition actually has).
    leftover_sectors = usable_sectors - (fat_aus + as400_total_aus) * args.au_size_sectors

    placed = []
    cursor = first_usable

    fat_end = cursor + fat_sectors - 1
    placed.append({
        "number": 1,
        "label": "FAT32/exFAT (SD card admin: zuluscsi.ini, firmware, tape/CD images)",
        "start": cursor,
        "end": fat_end,
    })
    cursor = fat_end + 1

    for i, p in enumerate(profiles):
        sectors = as400_allocated_sectors[i]
        if i + 1 == len(profiles):
            sectors += leftover_sectors

        end = cursor + sectors - 1
        placed.append({
            "number": i + 2,
            "label": p["name"],
            "start": cursor,
            "end": end,
        })
        cursor = end + 1

    if placed[-1]["end"] != last_usable:
        print(f"INTERNAL WARNING: computed layout ends at sector {placed[-1]['end']}, "
              f"expected {last_usable} (off by {last_usable - placed[-1]['end']}) -- "
              f"please report this, the arithmetic above has a bug", file=sys.stderr)

    print(f"{'Part':<4} {'Purpose':<68} {'Start sector':>14} {'End sector':>14} {'Size':>10}")
    for part in placed:
        mib = (part["end"] - part["start"] + 1) * 512.0 / (1024.0 * 1024.0)
        print(f"{part['number']:<4} {part['label']:<68} {part['start']:>14} {part['end']:>14} {mib:>8.1f}MiB")

    print()
    print(f"Every sector from {first_usable} to {last_usable} is allocated to one of the above "
          f"-- no unallocated space is left.")
    print()
    print("In gdisk:")
    print("  1. Before creating any partitions, set gdisk's own alignment to match: press")
    print(f"     x (expert menu), l (set sector alignment value), enter {args.au_size_sectors}, then m to")
    print("     return to the main menu.")
    print("  2. Create each partition above with its exact start/end sector from this table.")
    print("  3. Leave partition 1's type as the standard 0700 (Microsoft basic data).")
    print("  4. Set every AS/400 (PART:n, n>=2) partition's type GUID to")
    print(f"     {GPT_ZULUSCSI_PART_TYPE_GUID} so other operating systems leave it alone --")
    print("     see README.md's \"Raw sector-range and partition access\" section.")

    return 0


if __name__ == "__main__":
    sys.exit(main())
