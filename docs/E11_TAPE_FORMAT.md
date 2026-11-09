# E11 (unpadded) tape images — experimental firmware support

This branch adds opt-in **E11 record framing** to ZuluSCSI's existing SIMH `.tap` tape emulation. It is intended for an IBM 9404-B10 CISC OS/400 V2R3M0 Licensed Internal Code install test. **This draft upstream proposal has not yet been retested on the B10.**

## Configuration

In `zuluscsi.ini` for the tape target:

```ini
[SCSI]
System = AS400_CISC
AlignUnalignedAccesses = off
EnableUnitAttention = Yes

[SCSI0]
TapeImageFormat = E11
```

Keep the existing `Device=AS400_CISC`, `IMG0`, and other tape settings if already configured; the above shows the **new** line rather than a replacement of the section. All other `[SCSI5]` and `[SCSI6]` disk settings are unchanged.

**Allowed values:** `SIMH` (default; padded odd-length records) and `E11` (trailer immediately follows record data with **no padding**). The choice is per SCSI device, not per file, and persists when the target automatically cycles between volume images. A `.tap` filename alone cannot distinguish the variants. Malformed or unknown values log a warning and keep the current setting.

E11 uses the existing .tap record headers/trailers, tape marks, SPACE, READ and WRITE paths, with different stored length arithmetic for odd-sized records. The firmware also accepts the special terminal sequence found on the three larger archival volumes (repeated 32-bit `0x0000FFFF` followed by a final `0x00000000`) **only if it matches the entire remaining tail and is no longer than 1024 bytes**. This does not reinterpret ordinary 65,535-byte records.

## Grounding and validation

The four supplied AS/400 System Program V2R3M0 tapes were independently inspected:

| Metric | Total |
|---|---:|
| Valid data records | 23,344 |
| Odd-length unpadded E11 records | 8,161 |
| Filemarks | 1,467 |
| Data-record header/trailer mismatches | 0 |

Tape 2's first failing record begins at byte 17,278, has length 19,261, and has its matching trailer at byte 36,543. The old SIMH parser incorrectly expected it at 36,544.

To run host-side framing tests (read-only; not firmware execution):

```bash
python3 utils/test_e11_tapes.py
python3 utils/test_e11_tapes.py /path/to/tapes.zip
```

The test covers synthetic padded and unpadded tapes, forward and backward positioning, rejection of the wrong framing, and full record traversal of all four V2R3M0 ZIP members. The unusual trailing marker is also checked.

## Physical B10 acceptance criteria

1. Ensure **backups** of both 520-byte disk images. The IPL Function 24 LIC install is destructive.
2. Flash the ZuluSCSI Blaster UF2 built from this branch; verify the build's commit hash in its startup log.
3. Confirm the SCSI0 setting `TapeImageFormat = E11` and the original four unmodified tape images.
4. Start with tape 1 selected; run the B10 install.
5. Verify the MODE SELECT(6) disk command succeeds; verify medium-change UNIT ATTENTION suppression triggers at tape 1 -> tape 2; verify `SPACE(6)` passes the former tape-2 byte offset 17,278 without a `TAP record length mismatch`.
6. Observe any further SCSI CHECK CONDITION and front-panel SRCs. A passing host-side scan and firmware compile **do not prove successful IPL**.

The branch builds on the experimental B10 MODE SELECT(6) and CISC tape change compatibility fixes already committed in this fork. Until retested on the actual B10 this remains experimental; it is proposed as a draft for review rather than as hardware-validated firmware.
