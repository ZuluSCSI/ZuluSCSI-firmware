// Converts AS/400 disk images between tightly-packed logical sector layout
// and the "gapped" on-SD-card layout used by ZuluSCSI's AlignUnalignedAccesses
// setting (see README-as400.md). Two schemes, matching the two AS/400 logical
// block sizes this firmware supports:
//
//   cisc: 520-byte logical sectors, each padded out to its own 1024-byte
//         physical slot (1024 = 2*512, so every slot starts SD-sector-
//         aligned regardless of the 520-byte pitch).
//   ppc:  522-byte logical sectors, grouped 8-at-a-time into a 4608-byte
//         (9*512) physical group (8*522=4176 real bytes, 432 bytes of
//         trailing slack per group) -- OS/400's own paging already issues
//         8-sector-aligned requests, so this group boundary lines up with
//         real access patterns instead of padding every sector individually.
//
// Works equally on plain image files and raw partition/block device nodes --
// both are just byte-addressable targets opened with fopen()/fseeko(), no
// special-casing needed for either.
//
// Build: cc -O2 -o as400_gapconv as400_gapconv.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/stat.h>

#define _FILE_OFFSET_BITS 64

typedef enum { SCHEME_CISC, SCHEME_PPC } scheme_t;
typedef enum { MODE_INSERT, MODE_STRIP } gap_mode_t;

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s --scheme=cisc|ppc --mode=insert|strip [--sectors=N]\n"
        "          [--blocksize=N] --input=PATH --output=PATH\n"
        "\n"
        "  --scheme=cisc     520-byte sectors, 1024-byte padded slots\n"
        "  --scheme=ppc      522-byte sectors, 8-into-9-sector (4608-byte) groups\n"
        "  --mode=insert     tightly-packed logical image -> gapped physical layout\n"
        "  --mode=strip      gapped physical layout -> tightly-packed logical image\n"
        "  --sectors=N       number of logical AS/400 sectors to convert. Optional --\n"
        "                    auto-derived from the input's size when that's\n"
        "                    unambiguous (a plain file, exact multiple of the unit\n"
        "                    size for this scheme/mode); required otherwise -- notably\n"
        "                    always required for --scheme=ppc --mode=strip, since a\n"
        "                    full 8-sector group and a short trailing one occupy the\n"
        "                    identical physical size and can't be told apart, and\n"
        "                    always required when --input is a raw partition/block\n"
        "                    device, since its tail may be unrelated alignment\n"
        "                    padding rather than real data.\n"
        "  --blocksize=N     override logical sector size (default: 520 for cisc,\n"
        "                    522 for ppc)\n"
        "  --input/--output  PATH may be a regular file or a raw partition/block\n"
        "                    device node -- both are opened identically\n",
        prog);
}

// Returns 1 and fills *size_out with the file's size in bytes, or 0 on
// failure. Works for both regular files and (on most Unix systems) block
// device nodes, matching skip_input()'s own fseeko()-based approach above.
static int getFileSize(FILE *f, uint64_t *size_out)
{
    if (fseeko(f, 0, SEEK_END) != 0)
        return 0;
    off_t size = ftello(f);
    if (size < 0)
        return 0;
    if (fseeko(f, 0, SEEK_SET) != 0)
        return 0;
    *size_out = (uint64_t)size;
    return 1;
}

// Best-effort check for whether path names a raw partition/block device
// rather than a regular file -- if so, its tail may be unrelated alignment
// padding rather than real logical data, so auto-deriving --sectors from
// its size would risk silently converting extra garbage past the real
// disk's end (exactly the failure mode --sectors exists to avoid). Not
// available on every platform (no S_ISBLK) -- falls back to "assume it's
// a regular file" there, same as never checking at all.
static int isBlockDevice(const char *path)
{
#if defined(S_ISBLK)
    struct stat st;
    if (stat(path, &st) != 0)
        return 0;
    return S_ISBLK(st.st_mode) ? 1 : 0;
#else
    (void)path;
    return 0;
#endif
}

// One CISC slot: blockSize real bytes + padding up to 1024.
#define CISC_SLOT_SIZE 1024

// One PPC group: 8 logical sectors + padding up to 9 SD sectors.
#define PPC_GROUP_SECTORS 8
#define PPC_GROUP_PHYS_SIZE (9 * 512)

static int copy_exact(FILE *in, FILE *out, size_t n, unsigned char *buf, size_t bufcap)
{
    while (n > 0)
    {
        size_t chunk = n < bufcap ? n : bufcap;
        size_t got = fread(buf, 1, chunk, in);
        if (got != chunk)
        {
            fprintf(stderr, "Error: short read (wanted %zu, got %zu)\n", chunk, got);
            return -1;
        }
        if (fwrite(buf, 1, chunk, out) != chunk)
        {
            fprintf(stderr, "Error: short write (%zu bytes)\n", chunk);
            return -1;
        }
        n -= chunk;
    }
    return 0;
}

static int write_zeros(FILE *out, size_t n, unsigned char *buf, size_t bufcap)
{
    memset(buf, 0, bufcap < n ? bufcap : n);
    while (n > 0)
    {
        size_t chunk = n < bufcap ? n : bufcap;
        if (fwrite(buf, 1, chunk, out) != chunk)
        {
            fprintf(stderr, "Error: short write (%zu zero-fill bytes)\n", chunk);
            return -1;
        }
        n -= chunk;
    }
    return 0;
}

static int skip_input(FILE *in, size_t n)
{
    // fseeko works for both regular files and (on most Unix systems) block
    // device nodes; falls back to reading-and-discarding if seek fails
    // (e.g. a pipe), so this also works for non-seekable input.
    if (fseeko(in, (off_t)n, SEEK_CUR) == 0)
        return 0;

    unsigned char discard[4096];
    while (n > 0)
    {
        size_t chunk = n < sizeof(discard) ? n : sizeof(discard);
        if (fread(discard, 1, chunk, in) != chunk)
        {
            fprintf(stderr, "Error: short read while skipping %zu bytes\n", chunk);
            return -1;
        }
        n -= chunk;
    }
    return 0;
}

static int do_cisc(FILE *in, FILE *out, gap_mode_t mode, uint32_t blockSize, uint64_t sectors)
{
    unsigned char *buf = malloc(CISC_SLOT_SIZE);
    if (!buf) { fprintf(stderr, "Error: out of memory\n"); return -1; }

    uint32_t pad = CISC_SLOT_SIZE - blockSize;
    int rc = 0;

    for (uint64_t s = 0; s < sectors && rc == 0; s++)
    {
        if (mode == MODE_INSERT)
        {
            // logical: blockSize real bytes -> physical: blockSize + pad
            rc = copy_exact(in, out, blockSize, buf, CISC_SLOT_SIZE);
            if (rc == 0) rc = write_zeros(out, pad, buf, CISC_SLOT_SIZE);
        }
        else
        {
            // physical: blockSize real bytes + pad -> logical: blockSize real bytes
            rc = copy_exact(in, out, blockSize, buf, CISC_SLOT_SIZE);
            if (rc == 0) rc = skip_input(in, pad);
        }
    }

    free(buf);
    return rc;
}

static int do_ppc(FILE *in, FILE *out, gap_mode_t mode, uint32_t blockSize, uint64_t sectors)
{
    unsigned char *buf = malloc(PPC_GROUP_PHYS_SIZE);
    if (!buf) { fprintf(stderr, "Error: out of memory\n"); return -1; }

    int rc = 0;
    uint64_t remaining = sectors;

    while (remaining > 0 && rc == 0)
    {
        // Every group (including a short trailing one) occupies the full
        // 4608-byte physical span -- matches the firmware's own
        // group=sector/8 arithmetic, which doesn't special-case a partial
        // final group either.
        uint32_t sectors_in_group = remaining < PPC_GROUP_SECTORS ? (uint32_t)remaining : PPC_GROUP_SECTORS;
        uint32_t real_bytes = sectors_in_group * blockSize;
        uint32_t pad = PPC_GROUP_PHYS_SIZE - real_bytes;

        if (mode == MODE_INSERT)
        {
            rc = copy_exact(in, out, real_bytes, buf, PPC_GROUP_PHYS_SIZE);
            if (rc == 0) rc = write_zeros(out, pad, buf, PPC_GROUP_PHYS_SIZE);
        }
        else
        {
            rc = copy_exact(in, out, real_bytes, buf, PPC_GROUP_PHYS_SIZE);
            if (rc == 0) rc = skip_input(in, pad);
        }

        remaining -= sectors_in_group;
    }

    free(buf);
    return rc;
}

int main(int argc, char *argv[])
{
    const char *scheme_str = NULL, *mode_str = NULL, *input_path = NULL, *output_path = NULL;
    int64_t sectors = -1;
    int64_t blocksize_override = -1;

    for (int i = 1; i < argc; i++)
    {
        if (strncmp(argv[i], "--scheme=", 9) == 0) scheme_str = argv[i] + 9;
        else if (strncmp(argv[i], "--mode=", 7) == 0) mode_str = argv[i] + 7;
        else if (strncmp(argv[i], "--sectors=", 10) == 0) sectors = strtoll(argv[i] + 10, NULL, 0);
        else if (strncmp(argv[i], "--blocksize=", 12) == 0) blocksize_override = strtoll(argv[i] + 12, NULL, 0);
        else if (strncmp(argv[i], "--input=", 8) == 0) input_path = argv[i] + 8;
        else if (strncmp(argv[i], "--output=", 9) == 0) output_path = argv[i] + 9;
        else { fprintf(stderr, "Error: unrecognized argument '%s'\n", argv[i]); usage(argv[0]); return EXIT_FAILURE; }
    }

    if (!scheme_str || !mode_str || !input_path || !output_path)
    {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    scheme_t scheme;
    uint32_t default_blocksize;
    if (strcmp(scheme_str, "cisc") == 0) { scheme = SCHEME_CISC; default_blocksize = 520; }
    else if (strcmp(scheme_str, "ppc") == 0) { scheme = SCHEME_PPC; default_blocksize = 522; }
    else { fprintf(stderr, "Error: --scheme must be 'cisc' or 'ppc'\n"); return EXIT_FAILURE; }

    gap_mode_t mode;
    if (strcmp(mode_str, "insert") == 0) mode = MODE_INSERT;
    else if (strcmp(mode_str, "strip") == 0) mode = MODE_STRIP;
    else { fprintf(stderr, "Error: --mode must be 'insert' or 'strip'\n"); return EXIT_FAILURE; }

    uint32_t blockSize = blocksize_override > 0 ? (uint32_t)blocksize_override : default_blocksize;
    if (scheme == SCHEME_CISC && blockSize >= CISC_SLOT_SIZE)
    {
        fprintf(stderr, "Error: --blocksize (%u) must be smaller than the CISC slot size (%d)\n", blockSize, CISC_SLOT_SIZE);
        return EXIT_FAILURE;
    }
    if (scheme == SCHEME_PPC && (uint64_t)blockSize * PPC_GROUP_SECTORS >= PPC_GROUP_PHYS_SIZE)
    {
        fprintf(stderr, "Error: --blocksize (%u) leaves no room for padding in a %d-sector PPC group\n", blockSize, PPC_GROUP_SECTORS);
        return EXIT_FAILURE;
    }

    FILE *in = fopen(input_path, "rb");
    if (!in) { perror(input_path); return EXIT_FAILURE; }
    FILE *out = fopen(output_path, "wb");
    if (!out) { perror(output_path); fclose(in); return EXIT_FAILURE; }

    if (sectors < 0)
    {
        // PPC strip can never be auto-derived: a full 8-sector group and a
        // short trailing one occupy the exact same 4608-byte physical
        // span, so the size alone can't tell them apart.
        if (scheme == SCHEME_PPC && mode == MODE_STRIP)
        {
            fprintf(stderr, "Error: --sectors is required for --scheme=ppc --mode=strip "
                             "(a short trailing group can't be told apart from a full one by size alone)\n");
            fclose(in); fclose(out);
            return EXIT_FAILURE;
        }

        if (isBlockDevice(input_path))
        {
            fprintf(stderr, "Error: --sectors is required when --input is a raw partition/block device "
                             "(its tail may be unrelated alignment padding, not real data)\n");
            fclose(in); fclose(out);
            return EXIT_FAILURE;
        }

        uint64_t inputSize;
        if (!getFileSize(in, &inputSize))
        {
            fprintf(stderr, "Error: could not determine --input size to auto-derive --sectors\n");
            fclose(in); fclose(out);
            return EXIT_FAILURE;
        }

        // insert reads tightly-packed logical data (unit = blockSize);
        // strip (cisc only, ppc excluded above) reads the gapped physical
        // layout (unit = one whole slot).
        uint32_t unit = (mode == MODE_INSERT) ? blockSize : CISC_SLOT_SIZE;
        if (inputSize % unit != 0)
        {
            fprintf(stderr, "Error: --input size (%" PRIu64 " bytes) is not an exact multiple of %u bytes -- "
                             "cannot unambiguously auto-derive --sectors, pass it explicitly\n", inputSize, unit);
            fclose(in); fclose(out);
            return EXIT_FAILURE;
        }

        sectors = (int64_t)(inputSize / unit);
        fprintf(stderr, "%s: auto-derived --sectors=%" PRId64 " from --input size\n", argv[0], sectors);
    }

    fprintf(stderr, "%s: %s scheme, %s, %" PRId64 " sectors, blockSize=%u\n",
            argv[0], scheme_str, mode_str, sectors, blockSize);

    int rc = (scheme == SCHEME_CISC)
        ? do_cisc(in, out, mode, blockSize, (uint64_t)sectors)
        : do_ppc(in, out, mode, blockSize, (uint64_t)sectors);

    fclose(in);
    if (fclose(out) != 0) { perror(output_path); rc = -1; }

    if (rc == 0)
        fprintf(stderr, "%s: done\n", argv[0]);
    else
        fprintf(stderr, "%s: failed\n", argv[0]);

    return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
