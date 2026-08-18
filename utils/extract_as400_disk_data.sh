#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# extract_as400_disk_data.sh — Capture a real AS/400 disk's identity data
#
# Reads standard INQUIRY, all VPD pages the drive actually reports, the full
# MODE SENSE (all pages), READ CAPACITY, and LOG SENSE pages 0x00/0x30/0x31
# from a real AS/400-style SCSI disk, and appends the result as one profile
# section to a plain-text definitions file (default: as400_disk_definitions.txt,
# meant to be copied to the root of the ZuluSCSI's SD card).
#
# This is deliberately NOT XML/JSON — it's a minIni-compatible INI file, using
# the same space-separated-hex-byte convention that src/custom_vendor_inquiry.cpp
# already parses for zuluscsi.ini's [SCSIn] "spd"/"vpdXX" keys. Hex fields that
# exceed a safe INI line length (minIni's INI_BUFFERSIZE is 512 bytes) are split
# into NAME_0, NAME_1, ... chunks; the firmware does not yet reassemble these —
# capture now, load later.
#
# Requirements:  sg3_utils (sg_inq, sg_vpd, sg_modes, sg_logs, sg_raw), xxd
#                (READ CAPACITY is issued via a raw CDB through sg_raw, so
#                sg_readcap itself is not required)
#                Run as root or with appropriate /dev/sg* permissions.
#
# Usage:  ./extract_as400_disk_data.sh /dev/sgN <FeatureModel> [outfile]
#
#   /dev/sgN      — the sg device for the target disk
#   FeatureModel  — REQUIRED. The IBM Feature+Model designation for this disk
#                   (e.g. "6607-050"), used as the [section] name. This is NOT
#                   auto-detected: it is not present in the drive's own
#                   INQUIRY/VPD data (checked — every ASCII-shaped field in a
#                   known-real capture decodes to vendor/product/FRU/serial
#                   strings, never a Feature+Model pair). You know it from the
#                   physical unit's markings or system configuration records;
#                   the drive does not.
#   outfile       — file to append the profile to (default:
#                   as400_disk_definitions.txt in the current directory).
#                   Run once per physical drive; each run appends one section.
#
# Example:
#   ./extract_as400_disk_data.sh /dev/sg3 6607-050 as400_disk_definitions.txt
# ---------------------------------------------------------------------------

set -uo pipefail

DEV="${1:?Usage: $0 /dev/sgN <FeatureModel> [outfile]}"
LABEL="${2:?Usage: $0 /dev/sgN <FeatureModel> [outfile] -- FeatureModel is required, e.g. 6607-050}"
OUTFILE="${3:-as400_disk_definitions.txt}"

# Max raw bytes per emitted hex field before splitting into NAME_0, NAME_1, ...
# 3 chars per byte ("XX ") * 140 = ~420 chars of value, leaving comfortable
# headroom under minIni's 512-byte INI_BUFFERSIZE for "KEYNAME_N = " plus
# whatever margin minIni needs internally — measured 495/512 at 160 bytes/line
# in testing, too close for comfort given key names vary in length.
MAX_HEX_BYTES_PER_LINE=140

if [ ! -c "$DEV" ]; then
    echo "Error: $DEV is not a character device." >&2
    exit 1
fi

case "$LABEL" in
    \[*|*\]*)
        echo "Error: FeatureModel '$LABEL' must not contain '[' or ']' (it becomes an INI section name)." >&2
        exit 1
        ;;
esac

MISSING_TOOLS=()
for tool in sg_inq sg_vpd sg_modes sg_logs sg_raw xxd; do
    if ! command -v "$tool" &>/dev/null; then
        MISSING_TOOLS+=("$tool")
    fi
done
if [ "${#MISSING_TOOLS[@]}" -gt 0 ]; then
    echo "Error: missing tools: ${MISSING_TOOLS[*]}. Install sg3_utils and xxd." >&2
    exit 1
fi

SCRATCHDIR="$(mktemp -d)"
trap 'rm -rf "$SCRATCHDIR"' EXIT

SECTION_BODY="$SCRATCHDIR/section_body.txt"
: > "$SECTION_BODY"

warn() { echo "  ! $*" >&2; }
info() { echo "  - $*" >&2; }

# ---------------------------------------------------------------------------
# Run a command, capture stdout to a file, and report (not hide) failures.
# Returns the command's exit status; caller decides whether that's fatal.
# ---------------------------------------------------------------------------
run_capture() {
    local outfile="$1"; shift
    local errfile="$SCRATCHDIR/last_stderr.txt"
    if "$@" > "$outfile" 2>"$errfile"; then
        return 0
    else
        local status=$?
        warn "command failed (exit $status): $*"
        if [ -s "$errfile" ]; then
            sed 's/^/      /' "$errfile" >&2
        fi
        return "$status"
    fi
}

# ---------------------------------------------------------------------------
# Emit one or more "NAME = hex bytes" lines for a binary file, split so each
# line stays under MAX_HEX_BYTES_PER_LINE raw bytes. Appends to SECTION_BODY.
# ---------------------------------------------------------------------------
emit_hex_field() {
    local name="$1" file="$2"
    local size
    size=$(wc -c < "$file" | tr -d ' ')

    if [ "$size" -eq 0 ]; then
        echo "; $name: no data captured" >> "$SECTION_BODY"
        return
    fi

    local hex
    hex=$(xxd -p -c "$size" "$file" | tr -d '\n')
    # hex is now one long lowercase hex string, 2 chars per byte.

    if [ "$size" -le "$MAX_HEX_BYTES_PER_LINE" ]; then
        printf '%s = %s\n' "$name" "$(echo "$hex" | fold -w2 | paste -s -d ' ' -)" >> "$SECTION_BODY"
    else
        info "$name is $size bytes, splitting into ${name}_0, ${name}_1, ... (${MAX_HEX_BYTES_PER_LINE} bytes/chunk)"
        local chunk_hex_chars=$((MAX_HEX_BYTES_PER_LINE * 2))
        local idx=0 offset=0 total_hex_chars=${#hex}
        while [ "$offset" -lt "$total_hex_chars" ]; do
            local chunk="${hex:$offset:$chunk_hex_chars}"
            printf '%s_%d = %s\n' "$name" "$idx" "$(echo "$chunk" | fold -w2 | paste -s -d ' ' -)" >> "$SECTION_BODY"
            offset=$((offset + chunk_hex_chars))
            idx=$((idx + 1))
        done
        printf '%s_chunks = %d\n' "$name" "$idx" >> "$SECTION_BODY"
    fi
}

# Read a big-endian unsigned integer from a binary file at a byte offset.
be_uint() {
    local file="$1" offset="$2" len="$3"
    local hex
    hex=$(xxd -p -s "$offset" -l "$len" "$file" 2>/dev/null)
    [ -z "$hex" ] && { echo 0; return; }
    echo $((16#$hex))
}

echo "=== Capturing AS/400 disk profile '$LABEL' from $DEV ===" >&2

# ===== 1. Standard INQUIRY (full length, including vendor-specific tail) ===
info "Reading standard INQUIRY..."
INQ_RAW="$SCRATCHDIR/inquiry.bin"
run_capture "$INQ_RAW" sg_inq --raw "$DEV" || true

if [ -s "$INQ_RAW" ]; then
    ADDL=$(be_uint "$INQ_RAW" 4 1)
    FULL_LEN=$((ADDL + 5))
    if [ "$FULL_LEN" -gt 5 ]; then
        run_capture "$INQ_RAW" sg_raw -r "$FULL_LEN" "$DEV" 12 00 00 00 "$(printf '%02x' "$FULL_LEN")" 00 || true
    fi
fi

INQ_SIZE=$(wc -c < "$INQ_RAW" 2>/dev/null | tr -d ' '); INQ_SIZE=${INQ_SIZE:-0}
if [ "$INQ_SIZE" -eq 0 ]; then
    warn "Standard INQUIRY returned no data — capture will be incomplete."
fi
info "Standard INQUIRY: $INQ_SIZE bytes"

VENDOR="?" PRODUCT="?" REVISION="?"
if [ "$INQ_SIZE" -ge 36 ]; then
    VENDOR=$(dd if="$INQ_RAW" bs=1 skip=8 count=8 2>/dev/null | tr -d '\0' | sed -e 's/[[:space:]]*$//')
    PRODUCT=$(dd if="$INQ_RAW" bs=1 skip=16 count=16 2>/dev/null | tr -d '\0' | sed -e 's/[[:space:]]*$//')
    REVISION=$(dd if="$INQ_RAW" bs=1 skip=32 count=4 2>/dev/null | tr -d '\0' | sed -e 's/[[:space:]]*$//')
    info "Vendor='$VENDOR' Product='$PRODUCT' Revision='$REVISION'"
fi

# ===== 2. READ CAPACITY — the ground-truth size, independent of everything else
info "Reading capacity..."
CAP_RAW="$SCRATCHDIR/readcap.bin"
SECTORS=0 BLOCKSIZE=0
if run_capture "$CAP_RAW" sg_raw -r 8 "$DEV" 25 00 00 00 00 00 00 00 00 00; then
    if [ "$(wc -c < "$CAP_RAW" | tr -d ' ')" -eq 8 ]; then
        LAST_LBA=$(be_uint "$CAP_RAW" 0 4)
        BLOCKSIZE=$(be_uint "$CAP_RAW" 4 4)
        SECTORS=$((LAST_LBA + 1))
    fi
fi
if [ "$SECTORS" -eq 0 ]; then
    warn "READ CAPACITY(10) failed or returned nothing usable — Sectors/BlockSize will be 0, fill in by hand."
else
    info "Sectors=$SECTORS BlockSize=$BLOCKSIZE (=> $((SECTORS * BLOCKSIZE)) bytes exactly)"
fi

# ===== 3. VPD pages — read the REAL supported-page list, not a guessed one ==
info "Reading VPD page list (0x00)..."
VPD_LIST="$SCRATCHDIR/vpd00.bin"
run_capture "$VPD_LIST" sg_vpd --raw --page=0x00 "$DEV" || true

PAGE_CODES=()
if [ -s "$VPD_LIST" ]; then
    LIST_LEN=$(wc -c < "$VPD_LIST" | tr -d ' ')
    if [ "$LIST_LEN" -ge 4 ]; then
        # Bytes 2-3 are the page list length (big-endian) per SPC; only that
        # many bytes after the 4-byte header are actual page codes. Earlier
        # versions of this script read to EOF here, which could pull in
        # padding/garbage as bogus page codes.
        DECLARED_LEN=$(be_uint "$VPD_LIST" 2 2)
        AVAILABLE=$((LIST_LEN - 4))
        USE_LEN=$DECLARED_LEN
        if [ "$USE_LEN" -gt "$AVAILABLE" ]; then
            warn "VPD page-0 declares $DECLARED_LEN page codes but only $AVAILABLE bytes were read; truncating."
            USE_LEN=$AVAILABLE
        fi
        if [ "$USE_LEN" -gt 0 ]; then
            PAGE_CODES=($(xxd -p -s 4 -l "$USE_LEN" "$VPD_LIST" | tr -d '\n' | fold -w2))
        fi
    fi
fi

if [ ${#PAGE_CODES[@]} -eq 0 ]; then
    warn "Could not read VPD page 0x00's supported-page list; falling back to the known AS/400 page set."
    PAGE_CODES=(00 01 03 80 82 83 d1 d2)
fi
info "Supported VPD pages: ${PAGE_CODES[*]}"

# Plain indexed array, not an associative array: keeps this portable to bash
# 3.2 (macOS's default /bin/bash has no declare -A), not just bash 4+.
CAPTURED_VPD_PAGES=()
for pc in "${PAGE_CODES[@]}"; do
    PC_UP=$(echo "$pc" | tr '[:lower:]' '[:upper:]')
    VPD_FILE="$SCRATCHDIR/vpd_${pc}.bin"
    if run_capture "$VPD_FILE" sg_vpd --raw --page="0x${pc}" "$DEV"; then
        SZ=$(wc -c < "$VPD_FILE" | tr -d ' ')
        if [ "$SZ" -gt 0 ]; then
            info "VPD page 0x${PC_UP}: $SZ bytes"
            CAPTURED_VPD_PAGES+=("$pc")
        else
            warn "VPD page 0x${PC_UP}: empty response, skipping"
        fi
    fi
done

# ===== 4. MODE SENSE(6), all pages (0x3F) ===================================
info "Reading MODE SENSE (all pages)..."
MS_RAW="$SCRATCHDIR/modesense.bin"
# MODE SENSE(6): opcode=1A, DBD=0, PC=0 (current), page=3F, alloc=0xFF
run_capture "$MS_RAW" sg_raw -r 255 "$DEV" 1a 00 3f 00 ff 00 || \
    run_capture "$MS_RAW" sg_modes --raw --page=0x3f --six "$DEV" || true

MS_SIZE=$(wc -c < "$MS_RAW" 2>/dev/null | tr -d ' '); MS_SIZE=${MS_SIZE:-0}
info "MODE SENSE 0x3F: $MS_SIZE bytes"

# Decode CHS geometry for human review, if pages 0x03/0x04 are present in the
# response. This is informational only (redundant with the raw hex capture);
# it exists so a future reader doesn't have to hand-decode SCSI mode pages the
# way this session did.
GEOMETRY_COMMENT=""
if [ "$MS_SIZE" -ge 4 ]; then
    BLOCKDESCLEN=$(be_uint "$MS_RAW" 3 1)
    IDX=$((4 + BLOCKDESCLEN))
    SPT="" BPS_FROM_DESC="" CYL="" HEADS=""
    if [ "$BLOCKDESCLEN" -ge 8 ]; then
        BPS_FROM_DESC=$(be_uint "$MS_RAW" $((4 + 5)) 3)
    fi
    while [ "$IDX" -lt "$MS_SIZE" ]; do
        PAGECODE_BYTE=$(be_uint "$MS_RAW" "$IDX" 1)
        PAGECODE=$((PAGECODE_BYTE & 0x3F))
        [ "$PAGECODE" -eq 0 ] && break
        [ $((IDX + 2)) -gt "$MS_SIZE" ] && break
        PAGELEN=$(be_uint "$MS_RAW" $((IDX + 1)) 1)
        [ $((IDX + 2 + PAGELEN)) -gt "$MS_SIZE" ] && break
        DATA_OFF=$((IDX + 2))
        if [ "$PAGECODE" -eq 3 ] && [ "$PAGELEN" -ge 12 ]; then
            SPT=$(be_uint "$MS_RAW" $((DATA_OFF + 8)) 2)
        fi
        if [ "$PAGECODE" -eq 4 ] && [ "$PAGELEN" -ge 4 ]; then
            CYL=$(be_uint "$MS_RAW" "$DATA_OFF" 3)
            HEADS=$(be_uint "$MS_RAW" $((DATA_OFF + 3)) 1)
        fi
        IDX=$((DATA_OFF + PAGELEN))
    done
    if [ -n "$SPT" ] && [ -n "$CYL" ] && [ -n "$HEADS" ] && [ "$SPT" -gt 0 ] && [ "$HEADS" -gt 0 ]; then
        IMPLIED=$((CYL * HEADS * SPT))
        GEOMETRY_COMMENT="; Decoded from MODE SENSE pages 0x03/0x04: cylinders=$CYL heads=$HEADS sectors/track=$SPT (block size per descriptor: ${BPS_FROM_DESC:-unknown})
; => implied capacity $IMPLIED sectors. Compare against Sectors= below (from READ CAPACITY) —
; if these two numbers disagree, this drive's own MODE SENSE geometry is internally
; inconsistent the same way the firmware's built-in 09L4044 capture is (see project memory:
; project_as400_static_data_inconsistency.md). Trust READ CAPACITY / the INQUIRY+VPD identity,
; not the MODE SENSE geometry, when in doubt."
        info "MODE SENSE geometry: cyl=$CYL heads=$HEADS spt=$SPT => implied $IMPLIED sectors"
        if [ "$SECTORS" -gt 0 ] && [ "$IMPLIED" -ne "$SECTORS" ]; then
            warn "MODE SENSE geometry ($IMPLIED sectors) disagrees with READ CAPACITY ($SECTORS sectors) for this drive too."
        fi
    fi
fi

# ===== 5. LOG SENSE pages 0x00 / 0x30 / 0x31 ================================
info "Reading LOG SENSE pages..."
LS00_RAW="$SCRATCHDIR/logsense00.bin"
run_capture "$LS00_RAW" sg_logs --raw --page=0x00 "$DEV" || true
LS00_SIZE=$(wc -c < "$LS00_RAW" 2>/dev/null | tr -d ' '); LS00_SIZE=${LS00_SIZE:-0}
info "LOG SENSE page 0x00: $LS00_SIZE bytes"

LS30_RAW="$SCRATCHDIR/logsense30.bin"
run_capture "$LS30_RAW" sg_logs --raw --page=0x30 "$DEV" || true
LS30_SIZE=$(wc -c < "$LS30_RAW" 2>/dev/null | tr -d ' '); LS30_SIZE=${LS30_SIZE:-0}
info "LOG SENSE page 0x30: $LS30_SIZE bytes"
PL30=0 PLL30=0
if [ "$LS30_SIZE" -ge 4 ]; then
    PL30=$(be_uint "$LS30_RAW" 2 2)
    [ "$LS30_SIZE" -ge 8 ] && PLL30=$(be_uint "$LS30_RAW" 7 1)
fi

LS31_RAW="$SCRATCHDIR/logsense31.bin"
run_capture "$LS31_RAW" sg_logs --raw --page=0x31 "$DEV" || true
LS31_SIZE=$(wc -c < "$LS31_RAW" 2>/dev/null | tr -d ' '); LS31_SIZE=${LS31_SIZE:-0}
info "LOG SENSE page 0x31: $LS31_SIZE bytes"
PL31=0
[ "$LS31_SIZE" -ge 4 ] && PL31=$(be_uint "$LS31_RAW" 2 2)

# ===== 6. Assemble the [LABEL] section ======================================
{
    echo "Vendor = ${VENDOR:-?}"
    echo "Product = ${PRODUCT:-?}"
    echo "Revision = ${REVISION:-?}"
    echo "BlockSize = ${BLOCKSIZE:-0}"
    echo "Sectors = ${SECTORS:-0}"
    [ -n "$GEOMETRY_COMMENT" ] && echo "$GEOMETRY_COMMENT"
    echo ""
    echo "; --- Standard INQUIRY (full response) ---"
} >> "$SECTION_BODY"
[ "$INQ_SIZE" -gt 0 ] && emit_hex_field "SPD" "$INQ_RAW"

{
    echo ""
    echo "; --- VPD pages (space-separated hex bytes, minIni-parseable) ---"
} >> "$SECTION_BODY"
if [ ${#CAPTURED_VPD_PAGES[@]} -gt 0 ]; then
    for pc in "${CAPTURED_VPD_PAGES[@]}"; do
        PC_UP=$(echo "$pc" | tr '[:lower:]' '[:upper:]')
        emit_hex_field "VPD${PC_UP}" "$SCRATCHDIR/vpd_${pc}.bin"
    done
else
    echo "; no VPD pages captured" >> "$SECTION_BODY"
fi

{
    echo ""
    echo "; --- MODE SENSE(6), page 0x3F (all pages) ---"
} >> "$SECTION_BODY"
[ "$MS_SIZE" -gt 0 ] && emit_hex_field "ModeSense3F" "$MS_RAW"

{
    echo ""
    echo "; --- LOG SENSE pages ---"
} >> "$SECTION_BODY"
[ "$LS00_SIZE" -gt 0 ] && emit_hex_field "LogSense00" "$LS00_RAW"
if [ "$LS30_SIZE" -gt 0 ]; then
    emit_hex_field "LogSense30" "$LS30_RAW"
    {
        echo "LogSense30PageLength = $PL30"
        echo "LogSense30PageListLength = $PLL30"
    } >> "$SECTION_BODY"
fi
if [ "$LS31_SIZE" -gt 0 ]; then
    emit_hex_field "LogSense31" "$LS31_RAW"
    echo "LogSense31PageLength = $PL31" >> "$SECTION_BODY"
fi

{
    echo "; ==========================================================================="
    echo "; AS/400 disk profile '$LABEL', captured from $DEV on $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "; Feature+Model is not auto-detected — it is not present in this drive's own"
    echo "; INQUIRY/VPD data (checked; see project memory for the reasoning). Confirm"
    echo "; this label is actually correct for the physical unit before relying on it."
    echo "; ==========================================================================="
    echo "[$LABEL]"
    cat "$SECTION_BODY"
    echo ""
} >> "$OUTFILE"

echo "" >&2
echo "=== Summary ===" >&2
echo "Vendor/Product/Revision: '$VENDOR' / '$PRODUCT' / '$REVISION'" >&2
echo "Capacity: $SECTORS sectors x $BLOCKSIZE bytes = $((SECTORS * BLOCKSIZE)) bytes" >&2
echo "VPD pages captured: ${#CAPTURED_VPD_PAGES[@]}" >&2
echo "MODE SENSE 0x3F: $MS_SIZE bytes" >&2
echo "LOG SENSE pages: 0x00=${LS00_SIZE}B 0x30=${LS30_SIZE}B 0x31=${LS31_SIZE}B" >&2
echo "" >&2
echo "Appended profile [$LABEL] to $OUTFILE" >&2
echo "Copy $OUTFILE to the root of the ZuluSCSI's SD card as as400_disk_definitions.txt" >&2
echo "(the firmware does not yet load this file — capture now, loader is future work)." >&2
