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

There is a shell-script `utils/extract_as400_disk_data.sh` in the original source tree on GitHub to generate more *as400_disk_definitions.txt* entries from real disks connected to a SCSI controller when ran under Linux. With that, and a sector copy, you can migrate your real disks to Zulu SCSI, keeping disk metadata and serial numbers intact. Example command line for copying a disk's data: `sg_dd blk_sgio=1 if=/dev/sg0 bs=520 of=outfile_520.dd verbose=2 sync=1`.

Caveats:

- Writes to the emulated disk are very slow compared to reads. This is most apparent with PPC platforms.

Fully tested with Firmware v2026.08.27RC1, 9401-150, V4R4, V5R2.

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
