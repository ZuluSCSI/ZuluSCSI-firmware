#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# extract_as400_tape_data.sh --- Capture a real AS/400-era tape drive's
# identity and mode-page data, one run per cartridge/condition.
#
# Reads standard INQUIRY, the full MODE SENSE (all pages, 0x3F), and MODE
# SENSE page 0x10 (Device Configuration) alone from a real SCSI sequential
# (tape) device, and appends the result as one labeled, diffable block to a
# plain-text capture file (default: as400_tape_captures.txt).
#
# Intended use: run once per condition (blank cartridge, a cartridge known
# to be recorded at a specific density, etc.) against the SAME physical
# drive, then diff the blocks by eye --- whatever byte(s) actually change
# between runs are the real density/format-indicating field(s); anything
# that stays constant isn't, whatever a spec table might claim.
#
# Deliberately does NOT capture LOG SENSE or attempt VPD/EVPD pages beyond
# a courtesy check of page 0x00 --- LOG SENSE support for AS/400 tape is a
# deliberately deferred feature (see project memory), and CISC-era drives
# are already confirmed to have no VPD/EVPD support at all; capturing it
# unconditionally would just add noise for this specific diagnostic. If a
# real capture for a brand-new tape identity (INQUIRY+VPD, for compiling
# into src/as400_tape_values.cpp) is needed later, that's a separate,
# fuller capture --- this script is scoped to the density/mode-page
# question at hand.
#
# Requirements:  sg3_utils (sg_inq, sg_vpd, sg_raw), xxd
#                Run as root or with appropriate /dev/sg* permissions.
#
# Usage:  ./extract_as400_tape_data.sh /dev/sgN Label [outfile]
#
#   /dev/sgN      --- the sg device for the target tape drive
#   Label         --- REQUIRED (unlike the disk script, there's no reliable
#                   VPD field to auto-derive a name from across arbitrary
#                   tape drives). Use something that identifies BOTH the
#                   drive and the condition, e.g. "QIC2000-blank",
#                   "QIC2000-cart-recorded-QIC1000".
#   outfile       --- file to append the block to (default:
#                   as400_tape_captures.txt in the current directory).
#
# Example:
#   ./extract_as400_tape_data.sh /dev/sg3 QIC2000-blank
#   ./extract_as400_tape_data.sh /dev/sg3 QIC2000-cart-recorded-QIC1000
#   diff <(grep '^ModeSense10' as400_tape_captures.txt | sed -n 1p) \
#        <(grep '^ModeSense10' as400_tape_captures.txt | sed -n 2p)
# ---------------------------------------------------------------------------

set -uo pipefail

DEV="${1:?Usage: $0 /dev/sgN Label [outfile]}"
LABEL="${2:?Usage: $0 /dev/sgN Label [outfile] --- Label is required, e.g. QIC2000-blank}"
OUTFILE="${3:-as400_tape_captures.txt}"

MAX_HEX_BYTES_PER_LINE=140

if [ ! -c "$DEV" ]; then
    echo "Error: $DEV is not a character device." >&2
    exit 1
fi

case "$LABEL" in
    \[*|*\]*)
        echo "Error: Label '$LABEL' must not contain '[' or ']' (it becomes a section marker)." >&2
        exit 1
        ;;
esac

MISSING_TOOLS=()
for tool in sg_inq sg_vpd sg_raw xxd; do
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

    if [ "$size" -le "$MAX_HEX_BYTES_PER_LINE" ]; then
        printf '%s = %s\n' "$name" "$(echo "$hex" | fold -w2 | paste -s -d ' ' -)" >> "$SECTION_BODY"
    else
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

be_uint() {
    local file="$1" offset="$2" len="$3"
    local hex
    hex=$(xxd -p -s "$offset" -l "$len" "$file" 2>/dev/null)
    [ -z "$hex" ] && { echo 0; return; }
    echo $((16#$hex))
}

echo "=== Capturing AS/400 tape condition '$LABEL' from $DEV ===" >&2

# ===== 1. Standard INQUIRY (sanity check --- identity should not change
# across cartridges; if it does, something else is going on) ===============
info "Reading standard INQUIRY..."
INQ_RAW="$SCRATCHDIR/inquiry.bin"
run_capture "$INQ_RAW" sg_inq --raw "$DEV" || true

ADDL=$(be_uint "$INQ_RAW" 4 1)
if [ ! -s "$INQ_RAW" ] || [ "$ADDL" -eq 0 ]; then
    info "sg_inq gave nothing usable, probing INQUIRY directly via sg_raw..."
    PROBE_RAW="$SCRATCHDIR/inquiry_probe.bin"
    if run_capture "$PROBE_RAW" sg_raw -o - -r 36 "$DEV" 12 00 00 00 24 00; then
        ADDL=$(be_uint "$PROBE_RAW" 4 1)
        if [ -s "$PROBE_RAW" ] && [ "$ADDL" -gt 0 ]; then
            cp "$PROBE_RAW" "$INQ_RAW"
        fi
    fi
fi

if [ "$ADDL" -gt 0 ]; then
    FULL_LEN=$((ADDL + 5))
    run_capture "$INQ_RAW" sg_raw -o - -r "$FULL_LEN" "$DEV" 12 00 00 00 "$(printf '%02x' "$FULL_LEN")" 00 || true
fi

INQ_SIZE=$(wc -c < "$INQ_RAW" 2>/dev/null | tr -d ' '); INQ_SIZE=${INQ_SIZE:-0}
if [ "$INQ_SIZE" -eq 0 ]; then
    warn "Standard INQUIRY returned no data --- try manually: sg_raw -v -r 36 $DEV 12 00 00 00 24 00 | xxd"
fi
info "Standard INQUIRY: $INQ_SIZE bytes"

VENDOR="?" PRODUCT="?" REVISION="?"
if [ "$INQ_SIZE" -ge 36 ]; then
    VENDOR=$(dd if="$INQ_RAW" bs=1 skip=8 count=8 2>/dev/null | tr -d '\0' | sed -e 's/[[:space:]]*$//')
    PRODUCT=$(dd if="$INQ_RAW" bs=1 skip=16 count=16 2>/dev/null | tr -d '\0' | sed -e 's/[[:space:]]*$//')
    REVISION=$(dd if="$INQ_RAW" bs=1 skip=32 count=4 2>/dev/null | tr -d '\0' | sed -e 's/[[:space:]]*$//')
    info "Vendor='$VENDOR' Product='$PRODUCT' Revision='$REVISION'"
fi

# ===== 2. MODE SENSE(6), page 0x10 alone (Device Configuration) ============
# Same CDB shape the real 9402-400's own INZTAP sends: DBD=0, PC=0 (current),
# page=0x10, alloc=0x0C (12) --- matches what's actually been observed on the
# wire, so this capture is directly comparable to the AS/400-side traffic.
info "Reading MODE SENSE page 0x10 (Device Configuration)..."
MS10_RAW="$SCRATCHDIR/modesense10.bin"
run_capture "$MS10_RAW" sg_raw -o - -r 12 "$DEV" 1a 00 10 00 0c 00 || true
MS10_SIZE=$(wc -c < "$MS10_RAW" 2>/dev/null | tr -d ' '); MS10_SIZE=${MS10_SIZE:-0}
if [ "$MS10_SIZE" -eq 0 ]; then
    warn "MODE SENSE page 0x10 returned no data --- try manually: sg_raw -v -r 12 $DEV 1a 00 10 00 0c 00 | xxd"
fi
info "MODE SENSE page 0x10: $MS10_SIZE bytes"

# ===== 3. MODE SENSE(6), all pages (0x3F) --- catches anything not
# specifically anticipated (e.g. a data-compression or medium-partition
# page real hardware reports that Zulu doesn't currently emulate at all) ===
info "Reading MODE SENSE (all pages)..."
MS3F_RAW="$SCRATCHDIR/modesense3f.bin"
run_capture "$MS3F_RAW" sg_raw -o - -r 255 "$DEV" 1a 00 3f 00 ff 00 || true
MS3F_SIZE=$(wc -c < "$MS3F_RAW" 2>/dev/null | tr -d ' '); MS3F_SIZE=${MS3F_SIZE:-0}
if [ "$MS3F_SIZE" -eq 0 ]; then
    warn "MODE SENSE 0x3F returned no data --- try manually: sg_raw -v -r 255 $DEV 1a 00 3f 00 ff 00 | xxd"
fi
info "MODE SENSE 0x3F: $MS3F_SIZE bytes"

# ===== 4. VPD page 0x00 courtesy check --- just to record whether this
# drive supports VPD at all, not a full VPD capture (see header comment) ===
info "Checking VPD support (page 0x00)..."
VPD00_RAW="$SCRATCHDIR/vpd00.bin"
VPD_SUPPORTED="no"
if run_capture "$VPD00_RAW" sg_vpd --raw --page=0x00 "$DEV" && [ -s "$VPD00_RAW" ]; then
    VPD_SUPPORTED="yes"
fi
info "VPD supported: $VPD_SUPPORTED"

# ===== 5. Assemble the block ================================================
{
    echo "Vendor = ${VENDOR:-?}"
    echo "Product = ${PRODUCT:-?}"
    echo "Revision = ${REVISION:-?}"
    echo "VPDSupported = $VPD_SUPPORTED"
    echo ""
    echo "; --- Standard INQUIRY (full response) ---"
} >> "$SECTION_BODY"
[ "$INQ_SIZE" -gt 0 ] && emit_hex_field "SPD" "$INQ_RAW"

{
    echo ""
    echo "; --- MODE SENSE(6) page 0x10 (Device Configuration) alone ---"
    echo "; byte 0=page code, 1=page length, 2='CAP,CAF,Active Format',"
    echo "; 3=Active partition, 4=write buf full ratio, 5=read buf empty ratio,"
    echo "; 6-7=write delay time, 8=default gap size, 9=EOD auto-gen etc,"
    echo "; 10-12=buffer size at early warning, 13=data compression, 14=reserved"
} >> "$SECTION_BODY"
[ "$MS10_SIZE" -gt 0 ] && emit_hex_field "ModeSense10" "$MS10_RAW"

{
    echo ""
    echo "; --- MODE SENSE(6), page 0x3F (all pages) ---"
} >> "$SECTION_BODY"
[ "$MS3F_SIZE" -gt 0 ] && emit_hex_field "ModeSense3F" "$MS3F_RAW"

{
    echo "; ==========================================================================="
    echo "; AS/400 tape capture '$LABEL', captured from $DEV on $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "; Label should identify BOTH the drive and the cartridge/condition ---"
    echo "; compare ModeSense10/ModeSense3F byte-for-byte across labels to find"
    echo "; which field(s) actually change with density/format."
    echo "; ==========================================================================="
    echo "[$LABEL]"
    cat "$SECTION_BODY"
    echo ""
} >> "$OUTFILE"

echo "" >&2
echo "=== Summary ===" >&2
echo "Vendor/Product/Revision: '$VENDOR' / '$PRODUCT' / '$REVISION'" >&2
echo "MODE SENSE page 0x10: $MS10_SIZE bytes" >&2
echo "MODE SENSE 0x3F: $MS3F_SIZE bytes" >&2
echo "" >&2
echo "Appended block [$LABEL] to $OUTFILE" >&2
echo "Run again with a different Label per cartridge/condition, then diff the" >&2
echo "ModeSense10/ModeSense3F lines across blocks by eye." >&2
