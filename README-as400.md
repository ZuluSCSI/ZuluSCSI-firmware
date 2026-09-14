# AS/400 specific advice

Configuration of presented SCSI devices itself is usually automatic, according to the files and directories found on the SD card. The SD card has either a FAT32 or exFAT file system, the firmware understands both.

> **Note:** If storage containers ("images") in excess of 4 GiB in size are to be used, the card must be reinitialized ("formatted") with ExFAT.

It's usually easier to use the SD card with a card reader on a newer computer to create configurations, and files. The SD card is hotplug capable, and upon reinsertation, the ZuluSCSI reinitializes itself according to the SD card's content.

> **Note:** If you pull the card, the presented SCSI IDs are gone. What happens with the host is purely depends on the host OS.

From all of the device classes the Zulu offers, three are of general interest in the AS/400 world:

- Disk (DASD),
- CD/DVD (Optical),
- Tape.

> **Note:** RISC and PPC (PowerPC) are referring to the same class of processor.

---

## CD drive support

> **Note:** CISC era OS/400 has no CD-ROM support whatsoever.

To use,

- create directory `CD0`,
- populate with one or more readymade ISO images,
- use the serial console to attach image(s) to the drive.

No special handling for the ID in *zuluscsi.ini*.

Caveats:

- When you choose an image file through the USB port's media menu, make sure that you not only choose an image file, but afterwards *insert* it!
- If you replace a hardware CD drive, make sure to delete your old CD device file first, then IPL, check/set the new device name in DST, and have auto-configuration create the new device file during the following IPL.

Tested with Firmware 2026-08-07, 9401-150, V4R4, V5R2: IPL only so far, but that works.

## Disk drive support

To use,

- copy *as400_disk_definitions.txt* from the official GitHub repository to the SD card.
  - Because of serial numbers currently being hard coded, only one disk per type should be used for now!

### PPC

For PPC machines, 522 bytes per sectors have to be used, and appropriate disk profiles from the *as400_disk_definitions.txt* must be referred to.

```ini
[SCSI]
System = AS400_PPC

[SCSI6]
AS400_DiskProfile = "09L4044"
PrefetchBytes = 0
```

###  CISC (IMPI)

For CISC machines, 520 bytes per sectors have to be used, and appropriate disk profiles from the *as400_disk_definitions.txt* must be referred to.

```ini
[SCSI]
System = AS400_CISC

[SCSI6]
AS400_DiskProfile = "45G9463"
PrefetchBytes = 0
```

### Usage

When an *AS400_DiskProfile* is configured for a given SCSI ID, and the associated image cannot be found on the SD card, a new one is generated automatically, with the correct size. This takes some time, so wait until the activity LED stays unlit.

### Available disk profiles

*as400_disk_definitions.txt* carries several captures per real drive model; entries below are grouped by usable size and Feature Code, since several `as400_disk_definitions.txt` sections often describe the same physical drive model (multiple captured units, or minor firmware/certification variants). Entries with no Feature Code recorded (a real disk was captured, but which FC OS/400 would show for it isn't known) are omitted here — refer to *as400_disk_definitions.txt* directly for those.

| Usable size | Feature Code | `as400_disk_definitions.txt` sections | CISC padded size¹ | RISC/PPC padded size¹ |
|---|---|---|---|---|
| 957.7 MiB | #6104 | `55F9806` | 1.84 GiB | 1.04 GiB |
| 1001.5 MiB | #6601 | `45G9463`, `45G9463-1` | 1.93 GiB | 1.08 GiB |
| 2.04 GiB | #6606 | `86G9124`, `86G9124-5`, `74G6978-6` | 4.01 GiB | 2.26 GiB |
| 4.07 GiB | #6607 | `59H7001` | 7.99 GiB | 4.50 GiB |
| 8.35 GiB | #6713 | `59H6611` | 16.38 GiB | 9.21 GiB |
| 8.35 GiB | #6717 | `34L2279` | 16.38 GiB | 9.21 GiB |
| 16.67 GiB | #4318 | `08K0304` | 32.70 GiB | 18.39 GiB |

¹ **Status: implemented, not yet hardware-verified.** A per-device `AlignUnalignedAccesses` setting (`off`/`cisc`/`ppc`/`auto`, see below) pads or groups AS/400's 520/522-byte logical sectors so they land on the SD card's native 512-byte boundaries, trading SD card space for reduced access overhead — CISC pads each 520-byte sector out to its own 1024-byte slot; RISC/PPC groups 8 522-byte sectors into 9 SD-card sectors (4608 bytes). This column shows that padded size; with the setting off (the default), an image occupies its usable size directly (plus whatever slack the SD card's own filesystem allocates). **Images written under one resolved `AlignUnalignedAccesses` mode (off, `cisc`, `ppc`) are not compatible with either of the other two** -- each uses a different on-SD-card byte layout for the same logical disk. Simply changing the setting on an existing image will not reinterpret it correctly and will corrupt reads/writes. Converting an existing image between modes requires the `utils/as400_gapconv` tool (below), or recreating the image from scratch.

```ini
[SCSI6]
AS400_DiskProfile = "09L4044"
AlignUnalignedAccesses = auto
```

`AlignUnalignedAccesses` is set per SCSI ID. `auto` picks the scheme matching that ID's actual block size (520→`cisc`, 522→`ppc`, anything else→`off`) -- the recommended value for any profile-based device, since it can't drift out of sync with the profile the way spelling out `cisc`/`ppc` by hand can (a real, hardware-hit mistake during this feature's own testing: `PART:n` on a CISC profile with `AlignUnalignedAccesses` left unset entirely failed outright, since `RAW:`/`PART:` has never supported a non-512-byte-multiple block size without it). Spell out `cisc`/`ppc` explicitly only if you deliberately want gapping off for a particular profile despite its block size, or vice versa. **`AlignUnalignedAccesses` is never turned on implicitly just because `AS400_DiskProfile` is set** -- an existing, un-gapped image must keep working unmodified after an upgrade; it only ever takes effect when written explicitly. Compiles clean and passes standalone host-side tests cross-checked against `utils/as400_gapconv` byte-for-byte, but **has not yet been run against real AS/400 hardware** -- verify carefully (and back up any existing images first) before relying on it.

There is a shell-script `utils/extract_as400_disk_data.sh` in the original source tree on GitHub to generate more *as400_disk_definitions.txt* entries from real disks connected to a SCSI controller when ran under Linux. With that, and a sector copy, you can migrate your real disks to Zulu SCSI, keeping disk metadata and serial numbers intact. Example command line for copying a disk's data: `sg_dd blk_sgio=1 if=/dev/sg0 bs=520 of=outfile_520.dd verbose=2 sync=1`.

Caveats:

- Writes to the emulated disk are very slow compared to reads. This is most apparent with PPC platforms.

Fully tested with Firmware v2026.08.27RC1, 9401-150, V4R4, V5R2.

#### `utils/as400_gapconv` -- converting images for `AlignUnalignedAccesses`

A small standalone host-side C tool (not part of the firmware build; compile with `cc -O2 -o as400_gapconv utils/as400_gapconv.c`) that converts an AS/400 disk image between the tightly-packed logical-sector layout and the "gapped" physical layout `AlignUnalignedAccesses` will use on the SD card. Works identically on a plain image file or a raw partition/block-device node -- both are just opened as a byte stream, no special-casing.

```
as400_gapconv --scheme=cisc|ppc --mode=insert|strip [--sectors=N] \
              [--blocksize=N] --input=PATH --output=PATH
```

- `--scheme=cisc` -- 520-byte logical sectors, each padded to its own 1024-byte physical slot.
- `--scheme=ppc` -- 522-byte logical sectors, grouped 8-at-a-time into a 4608-byte (9-SD-sector) physical group, matching OS/400's own 8-sector-aligned paging.
- `--mode=insert` -- tightly-packed logical image (what you have today) → gapped physical layout.
- `--mode=strip` -- gapped physical layout → tightly-packed logical image (e.g. before moving an image to a card/setting where `AlignUnalignedAccesses` is off).
- `--sectors=N` -- the disk's logical sector count (from its `as400_disk_definitions.txt` profile). Optional -- auto-derived from `--input`'s size when that's unambiguous (a plain file whose size is an exact multiple of the relevant unit size). Always required for `--scheme=ppc --mode=strip` (a full 8-sector group and a short trailing one occupy the identical physical size, so there's no way to tell them apart from size alone) and whenever `--input` is a raw partition/block device (its tail may be unrelated alignment padding, not real data).
- `--blocksize=N` -- override the logical sector size; defaults to 520 (`cisc`) or 522 (`ppc`).

#### `utils/as400_part_planner` -- computing exact partition boundaries for `gdisk`

Getting a `PART:n` partition's size exactly right by hand is impractical --
it needs to be the gapped (not logical) size, rounded up to the SD card's
own preferred AU_SIZE boundary, and every partition after it needs its own
start to land on an AU boundary too. This tool does that arithmetic and
prints ready-to-type `gdisk` sector numbers instead. It never drives
`gdisk` itself.

Not part of the firmware build; a standalone Python 3 script (standard
library only, nothing to install or compile -- run it directly):

```
utils/as400_part_planner.py --total-sectors=N --au-size-sectors=N \
                             --profile=NAME [--profile=NAME ...]
```

- `--total-sectors=N` -- the SD card's total sector count (`gdisk -l /dev/sdX` prints this as `Disk /dev/sdX: N sectors`).
- `--au-size-sectors=N` -- the card's preferred alignment, in 512-byte sectors -- read directly off the Zulu console's own boot log line `SD preferred alignment: N sectors (...)`.
- `--profile=NAME` -- an `as400_disk_definitions.txt` profile to place, by its section name. Repeat in the order you want them assigned `PART:2`, `PART:3`, ...
- `--definitions=PATH` -- path to `as400_disk_definitions.txt` (default: in the current directory).

Every requested profile is sized to its exact gapped requirement, rounded
up to a whole AU_SIZE unit (so every partition starts AU-aligned); the
remaining space becomes `PART:1`, a FAT32/exFAT admin partition sized to
whatever's left -- no unallocated space anywhere on the card. Output is a
plain table (partition number, purpose, start/end sector, size) plus a
reminder of the exact `gdisk` steps: setting its own alignment to match
(`x`, `l`, the AU value, `m`) before creating anything, and the
preliminary ZuluSCSI partition type GUID (see README.md's "Raw
sector-range and partition access" section) to set on each AS/400
partition.

## Tape drive support

Currently, two AS/400 specific tape drives are emulated:

- a CISC era QIC1000, FC #6343,
- a PCI/PPC era SLR5 drive.

To use,

- create directory `TP0`,
- populate with one or more 0 byte files named `*.tap`, e. g. by using the Unix `touch` command,
- use the Zulu's USB console to attach image(s) to the drive.

Without any *Device* parameter, a standard tape drive is emulated, with no special AS/400 quirks. This works with many PPC/PCI machines. However, the presence of a `TapeDensity` parameter seems necessary in *zuluscsi.ini*.
```ini
[SCSI0]
TapeDensity = 0x25
```

For trying an emulated SLR5 (PPC/PCI era) drive:
```ini
[SCSI0]
Device = "AS400_PPC"
```

For trying an emulated QIC1000 (CISC era) drive with an emulated QIC1000 cartridge:
```ini
[SCSI0]
Device = "AS400_CISC"
```

Caveats:

- When you choose an image file through the USB port's media menu, make sure that you not only choose an image file, but afterwards *insert* it!
- If you replace a hardware tape drive, make sure to delete your old tape device file first, then IPL, check/set the new device name in DST, and have auto-configuration create the new device file during the following IPL.
- The amended (with an explicit *Device* statement) tape code as of v2026.09.10 has not yet undergone extensive testing. Initial tests yield mixed results.

---

# Current state

As of firmware release v2026.09.10. All successful tests so far have been based on a ZuluSCSI wide board on a 9401-150, while the CISC tests have been done with a ZuluSCSI Blaster on a 9401-P02, and a Zulu Wide on 9401-P03.

We have to differ three hardware generations when considering Zulu vs. AS/400 over supported device types, resulting in the following matrix:

| Device | CISC/IMPI, 520 bytes/block disks | PPC/SPD, 520/522 bytes/block disks   | PPC/PCI, 522 bytes/block disks |
| ------ | -------------------------------- | ------------------------------------ | ------------------------------ |
| CD-ROM | Not supported                    | Works                                | Works                          |
| Tape   | Fails for `savlib` and more      | Fails/Lack of feedback               | Works                          |
| Disk   | Works                            | Fails/Lack of feedback               | Works                          |

There are success reports of more 150's working, and one of a 9406-270 becoming stuck with A6000244, *Contact was lost with device indicated*.
