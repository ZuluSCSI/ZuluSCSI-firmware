# AS/400 specific advice

This document is meant as supplement to point out AS/400 relevant configuration parameters in detail. Basic and generic ZuluSCSI handling is documented in the main [README](README.md).

Configuration of presented SCSI devices itself is usually automatic, according to the files and directories found on the inserted SD card. The SD card has either a FAT32 or exFAT file system, the firmware understands both.

> **Note:** If storage containers ("images as files") in excess of 4 GiB in size are to be used, the card must be reinitialized ("formatted") with ExFAT. This only pertains to *files*, not on direct access via partitions.

It's usually easier to use the SD card with a card reader on a separate computer to create configurations and files. The SD card is hotplug capable, and upon reinsertion, the ZuluSCSI reinitializes itself according to the SD card's content.

If you physically remove the  card, the presented SCSI IDs are gone. What the host makes out of this situation purely depends on the host OS.

From all of the device classes the Zulu offers, three are of general interest in the AS/400 world:

- Disk (DASD),
- CD/DVD (Optical),
- Tape.

> **Note:** RISC and PPC (PowerPC) are referring to the same class of processor.

For single-bus machines, the SCSI-IDs are often assigned like this:
- 6 = Load source disk
- 1 = Optical drive
- 0 = Tape drive

Other IDs can be used for additional DASD.

## Current state

As of firmware release v2026.09.10. All successful tests so far have been based on a ZuluSCSI wide board on a 9401-150, while the CISC tests have been done with a ZuluSCSI Blaster on a 9401-P02, and a Zulu Wide on 9401-P03.

We have to differ three hardware generations when considering Zulu vs. AS/400 over supported device types, resulting in the following matrix:

| Device | CISC/IMPI, 520 bytes/block disks | PPC/SPD, 520/522 bytes/block disks   | PPC/PCI, 522 bytes/block disks |
| ------ | -------------------------------- | ------------------------------------ | ------------------------------ |
| CD-ROM | Not supported                    | Works                                | Works                          |
| Tape   | Fails for `savlib` and more      | Fails for `savlib` and more          | Works (generic device)         |
| Disk   | Works                            | Fails/Lack of feedback               | Works                          |

There are success reports of more 150's working, and one of a 9406-270 becoming stuck with A6000244, *Contact was lost with device indicated*, as well as another model 270 unable to IPL from emulated CD-ROM with a medium read error.

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

- set `AS400_DiskProfile` to one of the profiles in *as400_disk_definitions.txt*. The profiles in that file are built into the firmware, so nothing needs to be copied to the SD card to use them.
  - To add custom profiles, or to replace a built-in one, copy your own *as400_disk_definitions.txt* to the SD card. A profile in the custom store takes priority over the built-in profile of the same name; any profile the card does not have is still taken from the built-in set.
  - Multiple SCSI IDs sharing the same `AS400_DiskProfile` need a distinct `AS400_DiskSerialNumber` set on each, or OS/400 sees identical serial numbers and cannot tell the units apart -- see [Differentiating same-profile disks](#differentiating-same-profile-disks) below.

### PPC

For PPC machines, 522 bytes per sectors have to be used, and appropriate disk profiles from the *as400_disk_definitions.txt* must be referred to.

```ini
[SCSI]
System = AS400_PPC

[SCSI6]
AS400_DiskProfile = "09L4044"
PrefetchBytes=4160
```

`PrefetchBytes` is set to a value to absorb the majority of measured "happens anyway" accesses.

### CISC (IMPI)

For CISC machines, 520 bytes per sectors have to be used, and appropriate disk profiles from the *as400_disk_definitions.txt* must be referred to.

```ini
[SCSI]
System = AS400_CISC

[SCSI6]
AS400_DiskProfile = "45G9463"
PrefetchBytes = 4608
```

`PrefetchBytes` is set to a value to absorb the majority of measured "happens anyway" accesses.

### Defining AS/400 DASD

When an *AS400_DiskProfile* is configured for a given SCSI ID, and the associated image cannot be found on the SD card, a new one is generated automatically, with the correct size. This takes some time, so wait until the activity LED stays unlit.

> **Note:** A real disk can be supported by CISC and RISC platforms, notably the #6606 2 GiB "IBM 0662" based drives. Since VPD/mode page data is not yet persisted between ZuluSCSI boots, reformatting to a different sector size is moot, and has not been tested live!

*as400_disk_definitions.txt* carries several captures per real drive model; entries in the table below are grouped by usable size and Feature Code, since several `as400_disk_definitions.txt` sections often describe the same physical drive model. Entries with no Feature Code recorded (a real disk was captured, but which FC OS/400 would show for it isn't known) are omitted here — refer to *as400_disk_definitions.txt* directly for those.

| Usable size | Feature Code | `as400_disk_definitions.txt` sections | CISC padded size  | RISC/PPC padded size  |
|---|---|---|---|---|
| 957.7 MiB | #6104 | `55F9806` | 1.84 GiB | 1.04 GiB |
| 1001.5 MiB | #6601 | `45G9463`, `45G9463-1` | 1.93 GiB | 1.08 GiB |
| 2.04 GiB | #6606 | `86G9124`, `86G9124-5`, `74G6978-6` | 4.01 GiB | 2.26 GiB |
| 4.07 GiB | #6607 | `59H7001` | 7.99 GiB | 4.50 GiB |
| 8.35 GiB | #6713 | `59H6611` | 16.38 GiB | 9.21 GiB |
| 8.35 GiB | #6717 | `34L2279` | 16.38 GiB | 9.21 GiB |
| 16.67 GiB | #4318 | `08K0304` | 32.70 GiB | 18.39 GiB |

See below for the meaning of the *padded size*.

Check your machine model's individual platform restrictions which types of DASD is supported with your given machine and OS release.

### Differentiating same-profile disks

When two or more SCSI IDs use the same `AS400_DiskProfile`, set a distinct `AS400_DiskSerialNumber` on each so OS/400 sees separate units instead of a serial-number collision:

```ini
[SCSI6]
AS400_DiskProfile = "86G9124"
AS400_DiskSerialNumber = "01111111"

[SCSI5]
AS400_DiskProfile = "86G9124"
AS400_DiskSerialNumber = "02222222"
```

The value must be exactly 8 characters: hexadecimal digits only (`0`-`9`, `A`-`F`), with the first character always `0`. This isn't an arbitrary style choice -- the field is read back as a 28-bit binary value, not free text. A value that doesn't fit this shape shows as a masked serial (`00-********`) in DST's "Display Non-Configured Units" screen, and possibly yields an unusable device.

Without an `AS400_DiskSerialNumber` override, a named profile's own originally-captured serial is used verbatim and unchanged — good for a single disk of that profile, but two or more SCSI IDs sharing the same profile with no override will show the exact same serial to OS/400. An override is required, not just recommended, whenever a profile is used more than once.

### Performance optimization through `AlignUnalignedAccesses`

"Performance" in terms of AS/400 is better expressed in I/O requests per time frame, contrary to common platforms, where throughput is measured in amount of data transferred per time frame. The Single Level Store architecture is paging based and sustained large block transfers happen rarely. Hence, optimizing performance is a function of lessening storage latency.

SD cards have a natural block size for optimized (low latency) accesses. The baseline is 512 bytes, as spoken over a wide variety of platforms. This doesn't really map & match with the AS/400 platforms requirements of 520 or 520 bytes/block.

The per-device `AlignUnalignedAccesses` setting (`off`/`cisc`/`ppc`/`auto`, see below) pads or groups AS/400's 520/522-byte logical sectors so they land on the SD card's native 512-byte boundaries, trading SD card space for reduced access overhead.

```ini
[SCSI6]
AS400_DiskProfile = "09L4044"
AlignUnalignedAccesses = auto
```

The CISC setting pads each 520-byte sector out to a 1024-byte slot; the RISC/PPC setting groups 8 522-byte sectors into 9 SD-card sectors (4608 bytes). The latter was chosen because the majority of access patterns on RISC platforms matches the PowerPC CPU's page size: 4 KiB + 4× 10 bytes metadata. This creates "gaps" in the file, which still use up disk space, though.

> **Note:** Manually overriding `AlignUnalignedAccesses` for a given machine class has not yet been tested.

Images written under one `AlignUnalignedAccesses` mode (off, `cisc`, `ppc`) are not compatible with either of the other two! Each uses a different on-SD-card byte layout for the same logical disk. Simply changing the setting on an existing image will **not** reinterpret it and will corrupt reads/writes. Converting an existing image between modes requires the `utils/as400_gapconv` tool (see below), or recreating the image from scratch.

`AlignUnalignedAccesses` is set per SCSI ID. `auto` picks the scheme matching that ID's actual block size: 520 → `cisc`, 522 → `ppc`, anything else → `off`.

### Partitions vs. files

Further optimization of performance — at the expense of flexibility handling image files — can be achieved by not using images files, but defining "raw" partitions on a given SD card where the actual data is stored. This completely bypasses the file system layer needing extra CPU cycles on the ZuluSCSI boards, and again lowers latency.

```ini
[SCSI6]
AS400_DiskProfile = "09L4044"
Partition = 2
AlignUnalignedAccesses = auto
```

See the main [README](README.md) for more information regarding handling and assigning partitions to presented SCSI devices.

> **Note:** The usage of partitions makes using `AlignUnalignedAccesses` mandatory. Unaligned accesses, which are usually absorbed by the filesystem layer, are invalid for raw partition access.

Fully tested with prerelease firmware on 9401-150, V4R4 and 9401-P02, V2R3.

## Tape drive support

Currently, two AS/400 specific tape drives are emulated:

- a CISC era QIC1000, FC #6343,
- a PCI/PPC era SLR5, FC #6382.

> **Note:** Both emulation targets are currently **broken**. The only reliably working tape emulation is the standard (generic) tape device — no `Device` statement for the SCSI ID itself, and this only works with comparably new PPC/PCI machines.

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

---

## Helper tools

There are some AS/400 specific scripts and program sources in the original repository, which are described below.

### `utils/as400_gapconv` -- converting images for `AlignUnalignedAccesses`

A standalone tool written in C for performance reasons; see `AlignUnalignedAccesses` above. It allows to convert performance optimized data representations on disk:

- Adding and removing "gaps" for alignment
  - No direct conversion from CISC to RISC gapping, and vice versa
- Works for direct partition access
- Written for Linux, Windows support is unclear, running in WSL might work

It's not part of the firmware build. Compile with:

```shell
cc -O2 -o as400_gapconv utils/as400_gapconv.c
```

Parameters:

```shell
as400_gapconv --scheme=cisc|ppc --mode=insert|strip [--sectors=N] \
              [--blocksize=N] --input=PATH --output=PATH
```

- `--scheme` -- which type of "gapping" to be used
- `--mode` -- add or strip the given "gaps"
- `--sectors` -- the size of the input file/data. Auto-derived from `--input`'s size when that's unambiguous. That's essentially the `Sectors` value from a given entry in an `as400_disk_definitions.txt` profile.
- `--blocksize=N` -- override the logical sector size; defaults to 520 (`cisc`) or 522 (`ppc`).

Example:

- Convert unoptimized file to PowerPC optimized partition:

```shell
as400_gapconv --ppc --mode=insert --input=HD6.img --output=/dev/sdb2
```

### `utils/as400_part_planner` -- computing exact partition boundaries for partitions

Getting a `Partition = n` partition's size exactly right by hand is tedious -- it needs to be the gapped size, rounded up to the SD card's own preferred AU_SIZE boundary, and every partition after it needs its own start to land on an AU boundary too. This tool does that arithmetic and prints a table with sector numbers instead.

It's a standalone Python 3 script:

```shell
utils/as400_part_planner.py --total-sectors=N --au-size-sectors=N \
                             --profile=NAME [--profile=NAME ...]
```

- `--total-sectors=N` -- the SD card's total sector count (`gdisk -l /dev/sdX` prints this as `Disk /dev/sdX: N sectors`).
- `--au-size-sectors=N` -- the card's preferred alignment, in 512-byte sectors -- read directly off the Zulu console's own boot log line `SD preferred alignment: N sectors (…)`.
- `--profile=NAME` -- an `as400_disk_definitions.txt` profile to place, by its section name. Repeat in the order you want them assigned `Partition = 2`, `Partition = 3`, …
- `--definitions=PATH` -- path to `as400_disk_definitions.txt` (default: search current directory).

The remaining space becomes partition 1, the FAT32/exFAT admin partition sized to whatever's left.

For using `gdisk`, setting its own alignment to match helps spotting errors. Type `x`, `l`, the AU value, and `m` to return to the main menu.

---

## Memory pool considerations

If you use automatic performance optimization on OS/400, you might note that after migration to ZuluSCSI, the `*MACHINE` memory pool will inflate during periods of heavy I/O. Later, wenn normal workload processing continues, performance might suffer because of increased paging activity, until the performance adjuster shifts memory around accordingly.

This can be mitigated by running `wrkshrpool`, and pressing `F11` afterwards. In the shown screen, limits can be defined, to restrict boundless shifting of memory.

## Migration from real DASD

In practice, you might want to migrate a real installation from DASD to ZuluSCSI. To achieve this,

There is a shell-script `utils/extract_as400_disk_data.sh` in the original source tree on GitHub to generate *as400_disk_definitions.txt* entries from real disks connected to a SCSI controller when ran under Linux. With that, and a sector copy, you can migrate your real disks to ZuluSCSI, keeping disk metadata and serial numbers intact. Example command line for copying a disk's data:

```shell
sg_dd blk_sgio=1 if=/dev/sg0 bs=522 of=id6.dd verbose=2 sync=1
```

> **Note:** For CISC disks, use `bs=520`.

Another way of imaging disks is to use the ZuluSCSI's initiator mode. See [README](README.md#scsi-initiator-mode) for more information.
