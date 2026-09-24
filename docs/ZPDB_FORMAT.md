# ZPDB — ZuluSCSI Profile DataBase, on-flash format v1.0

A container for the `data/zulu_profiles/*.ini` drive profiles (and any other
key/value profile data of the same shape). It replaced the statically allocated
`g_custom_vpd`, `g_custom_spd` and `g_custom_modesense` tables in
[custom_vendor_inquiry.cpp](../../ZuluSCSI-firmware/src/custom_vendor_inquiry.cpp)
with lookups that read straight out of flash.

The store lives at a fixed offset in the external flash, in the 72 kB carved out
between the settings area and the ROM drive. The whole map is hardcoded in
[ZuluSCSI_platform.h](../../ZuluSCSI-firmware/lib/ZuluSCSI_platform_RP2MCU/ZuluSCSI_platform.h):

| offset  | size    | region                            |
|---------|---------|-----------------------------------|
| 0       | 1156 kB | bootloader + main firmware        |
| 1183744 | 16 kB   | settings area (reserved)          |
| 1200128 | 72 kB   | **profile store**                 |
| 1273856 | rest    | ROM drive, to the end of the chip |

The ROM drive's start is the fixed point: it has been at 1244 kB since before the two
data regions existed, so they were carved out of the firmware's old allocation rather
than pushing the ROM drive up. `program_flash_allocation` in `platformio.ini` was
shrunk to match, and a `static_assert` in `ZuluSCSI_platform.cpp` fails the build if
the linker's allocation and the map ever disagree.

The store is never read through the XIP window: reads go through
`platform_flash_read()`, which streams the flash device directly, so nothing is cached
on the way and a value read straight after a program is never stale.

Design rules, in the order they constrain everything else:

1. **Located once, read once.** A lookup returns a store-relative flash offset plus a
   byte length. The value is then read directly into the buffer the response is
   served from -- normally `scsiDev.data` -- so it is never staged in RAM in between.
2. **Everything is `uint32_t` aligned.** Every record and every value payload starts
   on a 4-byte boundary; every record length is a multiple of 4 — which is also
   exactly what the flash controller's streaming FIFO wants. A 64-bit value is
   therefore only guaranteed 4-aligned: `readU64()` takes it as two word loads,
   never as an `LDRD` off a cast pointer.
3. **A section is skipped by reading its 8-byte header.** Each section back-writes its
   own total length into its first two bytes, so a linear scan reads 8 bytes and adds
   per section — it never reads the keys of sections it isn't looking for. That
   matters more here than it would through a memory mapping: every skip that costs
   nothing is flash traffic that never happens.
4. **A section fits one 4096-byte flash page**, so the writer can assemble a whole
   section in one buffer and reject an oversized one before anything is committed.
5. **Three 4096-byte buffers are enough to write the whole store** (see *Writing*).

Everything is little-endian, matching RP2040/RP2350.

---

## Layout

```
+--------------------------- 32 B ---------------------------+
| zpdb_header                                                |
+------------------------------------------------------------+
| section[0]   (sect_len bytes, 4-aligned, <= 4096)          |
| section[1]   ...                                           |
| ...                                                        |
+------------------------------------------------------------+
```

Sections are packed back to back with no gaps and no index. Sections may straddle a
4096-byte flash page boundary; the reader addresses the store as a flat byte range,
so nothing there cares.

### Container header — 32 bytes at the store's base offset

| off | size | field          | meaning                                                   |
|----:|-----:|----------------|-----------------------------------------------------------|
|   0 |    4 | `magic`        | `0x42445A50` — the bytes `'Z' 'P' 'D' 'B'`                 |
|   4 |    2 | `version`      | `0x0100` = 1.0. Major in the high byte; a reader rejects an unknown major |
|   6 |    2 | `header_size`  | 32. First section starts here                              |
|   8 |    4 | `total_size`   | whole store incl. header, multiple of 4                    |
|  12 |    4 | `section_count`| number of sections                                         |
|  16 |    4 | `crc32`        | CRC-32 (zlib polynomial, reflected) over `[header_size, total_size)` |
|  20 |    4 | `build_epoch`  | Unix time of the build, for "is the store stale?" logging   |
|  24 |    4 | `flags`        | reserved, 0                                                |
|  28 |    4 | `reserved`     | reserved, 0                                                |

The CRC covers the section area only, so the header can be re-written (see *Writing*)
without recomputing anything.

### Section record — 8-byte header + name + entries

| off | size | field         | meaning                                                    |
|----:|-----:|---------------|------------------------------------------------------------|
|   0 |    2 | `sect_len`    | **total** bytes of this section, header and padding included. Multiple of 4, `<= 4096`. This is the field a scan follows |
|   2 |    1 | `entry_count` | number of entries, `<= 255`                                 |
|   3 |    1 | `name_len`    | bytes in `name`, excluding the NUL                          |
|   4 |    4 | `name_hash`   | FNV-1a-32 of the ASCII-uppercased name                      |
|   8 |    n | `name`        | NUL-terminated, zero-padded to a multiple of 4              |
|     |      | entries       | `entry_count` entry records, back to back                   |

`name_hash` makes a scan a `uint32_t` compare per section; the name itself is only
compared (case-insensitively) on a hash hit. The name is NUL-terminated so it can be
handed straight to `logmsg()` or a menu without a copy.

### Entry record — 12-byte header + key + value

| off | size | field       | meaning                                                      |
|----:|-----:|-------------|--------------------------------------------------------------|
|   0 |    2 | `entry_len` | **total** bytes of this entry, padding included. Multiple of 4 |
|   2 |    2 | `val_off`   | byte offset from the start of this entry to the value payload  |
|   4 |    4 | `key_hash`  | FNV-1a-32 of the ASCII-uppercased key                          |
|   8 |    2 | `val_len`   | **length of the value in bytes**                               |
|  10 |    1 | `type`      | see below                                                      |
|  11 |    1 | `key_len`   | bytes in `key`, excluding the NUL                              |
|  12 |    n | `key`       | NUL-terminated, zero-padded to a multiple of 4                 |
|`val_off`| `val_len` | value | payload, zero-padded to a multiple of 4              |

`val_off` is derivable from `key_len`, but it is stored so that locating the value is
one add off the entry's own offset, with no arithmetic on the key length:

```c
uint32_t where = entry.offset + entry.val_off;   // store-relative flash offset
uint32_t len   = entry.val_len;                  // bytes
```

### Types

| id | name             | `val_len` | payload                                                    |
|---:|------------------|-----------|-------------------------------------------------------------|
|  0 | `ZPDB_TYPE_NULL` | 0         | none. The key exists with no value (`Feature =` with nothing after it) |
|  1 | `ZPDB_TYPE_LONG` | 4         | `int32_t`                                                    |
|  2 | `ZPDB_TYPE_U64`  | 8         | 64-bit integer, 4-aligned — read as two words                |
|  3 | `ZPDB_TYPE_DATA` | n         | raw binary, the decoded form of a hex-string INI value       |
|  4 | `ZPDB_TYPE_STR`  | n         | characters, **not** counting the terminator. The builder always writes at least one NUL of padding, so the payload is directly usable as a `const char *` |

A null entry is *present but empty* and is deliberately distinct from an absent key:
`Feature =` in a profile means "this drive was captured with no feature string",
which is not the same as "this profile has nothing to say about features".

---

## INI → ZPDB rules

The builder is `zpdb_build.py`. Section names come from the `[...]` headers, which in
`data/zulu_profiles/` match the file name (`[34L2279]` in `34L2279.ini`). Section
names must be unique across all input files; a collision is a build error, not a
silent last-one-wins.

### Chunk collapsing

Values longer than a comfortable INI line are split in the source files. Two
different limits apply, depending on how a profile reaches the store:

- **512 bytes, via minIni.** A profile that arrives inside
  `as400_disk_definitions.txt` is split out by `ini_browse()`, whose
  `INI_BUFFERSIZE` is 512. That buffer holds the section name, the key name *and*
  the value together, so the budget for a value is 512 minus both names -- 483
  bytes for `ModeSense3F_0` under `[59H7001-170W-1]`. The longest line shipped in
  `as400_disk_definitions.txt` uses 448 of the 512, leaving 64 bytes of margin.
  `utils/extract_as400_disk_data.sh` is what keeps it there, chunking at 140 hex
  bytes (~420 characters) per line.
- **1024 bytes, via the store's own reader.** A `.ini` dropped straight into
  `/zulu_profiles` is parsed by `zpdb_profiles.cpp`, which never goes through
  minIni and reads up to `ZPDB_INI_LINE_MAX`. The extra headroom is there to
  *detect* an over-long line and report it rather than silently truncate.

So 512 is the number to author against: it is the only one both paths satisfy.

```ini
ModeSense3F_0 = c3 00 10 08 01 06 1d 54 00 00 02 0a 81 0a c4 0b ...
ModeSense3F_1 = 00 00 00 00 00 73 6d 01 00 00 00 00 00 00 10 0c ...
ModeSense3F_chunks = 2
```

`Key_0`, `Key_1`, … `Key_N` are concatenated **in index order** into a single entry
keyed `ModeSense3F`, whose `val_len` is the total byte count (274 for the example
above). The runtime never sees the split.

`Key_chunks` is **dropped**: it is fully implied by the merged `val_len`. The builder
still reads it, and warns if it disagrees with the number of `_N` keys actually
present, or if the indices are not a contiguous `0..N` run — a mismatch means the
capture is truncated and should be fixed at the source rather than silently encoded.

A key must not appear both plain and chunked in the same section (warned). A key
appearing twice keeps the last value and warns — `55F9806.ini` does this with
`Feature`, which is how the rule was chosen.

### Typing

Syntax alone cannot type these files. `Revision = 1644` and `Revision = 5959` are
ASCII revision strings; `LogSense31PageLength = 0` is a number; `Revision = s 62`
contains a space. So known keys are typed by a name schema and only unknown keys fall
back to inference:

| key pattern                                        | type   |
|----------------------------------------------------|--------|
| `Vendor` `Product` `Revision` `Feature`            | str    |
| `Sectors`                                          | u64    |
| `BlockSize` `*PageLength` `*PageListLength`        | long   |
| `SPD` `VPDxx` `ModeSensexx` `LogSensexx`           | data   |
| anything else                                      | inferred: empty → null; two or more `hh` hex tokens → data; integer → long/u64; otherwise str |

`Sectors` is u64 rather than long on purpose: a large-capacity image at 520/522-byte
blocks passes 2³¹ sectors, and the field is meant to survive that.

An empty value is `null` whatever the schema says for that key.

---

## Reading

```c
zpdbFlashInit();                       // check the store's flash region
store.open(zpdbFlashRead, nullptr, zpdbFlashSize());

zpdb_section_ref_t profile;
zpdb_entry_ref_t   spd;
store.findSection("34L2279", &profile);
store.findKey(profile, "SPD", &spd);

uint32_t len;
store.readValue(spd, scsiDev.data, sizeof(scsiDev.data), &len);   // flash -> response
```

Cost of a lookup: one 8-byte header read and an add per skipped section, one
`uint32_t` compare per candidate — the hash comes back in the header that the skip
already paid for, and the name itself is only read on a hash hit. Then the same per
entry within the one section that matched. For the 24 profiles in tree that is 24
header reads worst case to find a section, then at most 27 to find a key inside it.
No allocation, no RAM copy, no re-scan of the source text — the `minIni` path it
replaces re-reads the whole definitions file from the start for *every single key*.

What replaced the static tables:

| was                                       | is now                                               |
|-------------------------------------------|------------------------------------------------------|
| `g_custom_spd[id].data` / `.length`       | `zpdbReadSPD()` → straight into `scsiDev.data`        |
| `g_custom_vpd[i]` matched by `(id, page)` | `zpdbReadVPD()` — key `VPD%02X`                       |
| `g_custom_modesense[id]`                  | `zpdbReadModeSense()` — key `ModeSense3F`             |

The per-SCSI-ID dimension disappears from storage: a SCSI ID is *bound* to a section
once (`zpdbBindProfile()`), and all that is kept per target is a 16-byte section
reference. N IDs on the same profile cost nothing extra.

When no profile is bound, the built-in AS/400 disk and tape identities are served the
same way — copied into `scsiDev.data` directly from the const arrays in
`as400_values.h` / `as400_tape_values.h`, with `g_builtin[]` (one byte per target)
recording which identity applies. The per-ID `[SCSI<X>] vpdXX=` / `spd=` hex overrides
were removed along with the tables; profiles are the way to customise inquiry data.

### Identity injection

`injectSerial()` and `injectPartNumber()` patch bytes inside VPD pages
0x01/0x80/0x82/0x83/0xD1 and inside the SPD. Those patches are applied **after** the
page has been read into `scsiDev.data` — not baked into a per-target copy at load
time. That is what lets the page stay in flash: nothing needs a writable copy of it
just to carry a unique serial or FRU.

Two functions do it, because the two sources need different offsets:
`applyBuiltinIdentityFields()` uses the fixed offsets of the one built-in capture and
runs unconditionally, so the generated per-ID serial keeps the IDs that share that
identity apart; `applyProfileIdentityFields()` derives each offset from the page's own
length / descriptor-length bytes, so it works on any capture, and it only runs when
that SCSI ID has an `AS400_DiskSerialNumber` set. Without one a profile's own captured
serial is served unchanged, which is why two IDs sharing a profile each need their
own — see README-as400.md, "Differentiating same-profile disks".

## Writing — three 4096-byte buffers

The store's 72 kB region is erased whole before a rebuild, then programmed one
4096-byte page at a time. The writer is built around three buffers of that size:

| buffer            | role                                                                 |
|-------------------|----------------------------------------------------------------------|
| **section**       | the section currently being assembled. Entries are appended into it; `sect_len` is back-written to its offset 0 when the section is closed. Because the buffer *is* the 4096-byte limit, an oversized section is caught here and rejected before any flash is touched |
| **page**          | the flash page currently being filled. Closed sections are memcpy'd in; when it fills, it is programmed and the remainder of the section carries over |
| **page-0 shadow** | page 0 is held back until the end, because `total_size`, `section_count` and `crc32` are only known once the last section is written. The header — and with it the magic — is programmed **last**, so a store interrupted mid-write reads as absent rather than as valid-but-truncated |

All three are carved out of the scratch buffer the caller lends `zpdbProfilesInit()`
for the length of the call — in this firmware that is `scsiDev.data`, which is only
free of host data on the power-on path — so they cost nothing once boot is over and
the store owns no buffer of its own.

The CRC is accumulated incrementally as section bytes pass through, so no third pass
over the data is needed. After each page is programmed it is read back and compared
256 bytes at a time off the stack — no fourth 4 KB buffer — and a mismatch is logged
and fails the write, never silently ignored.

Worst case RAM during a rebuild: 3 × 4096 plus a `ZPDB_INI_LINE_MAX` line buffer
(1 KB) and 1 KB of hex
decode scratch, all transient, and none of it allocated by the store itself.

The permanently resident static tables it retired, measured symbol by symbol against
the same tree without this change: 16,569 bytes on ZuluSCSI_RP2040, 16,213 on
ZuluSCSI_Blaster, 32,061 on ZuluSCSI_Wide (the 8-target boards carry 48 VPD entries,
the 16-target board 96). The code and tooling cost 8.5-10.8 kB of flash in return, on
boards with megabytes of it.

## Boot ingest

`zpdbProfilesInit()` runs once per `reinitSCSI()`, after the card is mounted and
before any target configuration is read. It looks in **`/zulu_profiles`** for `.ini`
files:

- **No `.ini` files** — the store already in flash is used as it stands. This is the
  normal case after the first boot.
- **One or more** — the whole region is erased, then every file is parsed into a
  fresh store.

Each file is then moved out of the way: into **`/zulu_profiles/loaded`** if it was
stored, or **`/zulu_profiles/failed`** if it was not, so the next boot sees an empty
directory and keeps the store it just built. Files are taken one at a time with a
fresh directory scan each round, which is what keeps the iteration valid while files
are being renamed out from under it.

A file fails on a line longer than `ZPDB_INI_LINE_MAX` (1024) characters -- note
that a profile routed through `as400_disk_definitions.txt` is capped at 512 long
before it gets here, see "Chunk collapsing" above -- a missing `]` or `=`, a key
outside any section, a hex value that isn't a list of hex byte pairs, chunk indices
out of order, a profile name another file already used, or a section that would
exceed 4096 bytes. A half-built section is dropped rather than committed; sections the
file already completed stay in the store, and the log says so.
