# Finite tape image sequence for AS/400 CISC testing

The default ZuluSCSI behavior is to wrap IMG3 -> IMG0 when the host ejects the last
image. During an IBM 9404-B10 LIC/OS/400 V2R3M0 test this repeated the four-image
set many times instead of identifying the missing next volume.

Use the following opt-in setting on the sequential target:

```ini
[SCSI0]
Type = 5
Device = "AS400_CISC"
TapeImageFormat = E11
TapeImageLoop = 0
IMG0 = "V2R3M0-TP0/01-AS400_System_Program_V2_R03M00_TAPE1.tap"
IMG1 = "V2R3M0-TP0/02-AS400_System_Program_V2_R03M00_TAPE2.tap"
IMG2 = "V2R3M0-TP0/03-AS400_System_Program_V2_R03M00_TAPE3.tap"
IMG3 = "V2R3M0-TP0/04-AS400_System_Program_V2_R03M00_TAPE4.tap"
```

- Default `TapeImageLoop=1` preserves the previous wraparound behavior.
- For a sequential device with `TapeImageLoop=0`, IMG0 -> IMG1 -> IMG2 -> IMG3
  still switches normally.
- On the eject after IMG3, ZuluSCSI logs `Tape image sequence exhausted`,
  closes the previous image, and leaves the medium absent. It does not
  remount IMG0 even if the host sends START/INQUIRY again.
- This is a diagnostic option, **not** a guarantee that the AS/400 installer
  needs exactly four tapes. If it requests a fifth volume, inspect the B10 SRC
  and SCSI sense response rather than automatically repeating tape 1.

Expected real-hardware checks: one transition per image, no `Switching to next
image ... TAPE1` after the last volume, and `NOT READY / MEDIUM NOT PRESENT`
on reads after the sequence is exhausted. Observe whether the B10 performs
disk writes or requests another installation tape.

The physical firmware needs to be rebuilt and tested before treating this
behavior as validated.
