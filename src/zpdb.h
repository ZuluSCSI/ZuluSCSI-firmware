/**
 * Copyright (C) 2026 Rabbit Hole Computing LLC
 *
 * This file is part of ZuluSCSI
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
**/

// ZPDB -- ZuluSCSI Profile DataBase, a key/value store for drive profiles held
// in flash. See docs/ZPDB_FORMAT.md for the on-flash layout; the
// short version:
//
//   - every record and value payload is uint32_t aligned, every record
//     length is a multiple of 4
//   - each section carries its own total length in its first two bytes, so
//     a scan skips a section by reading 8 bytes and adding -- it never
//     reads the keys of sections it isn't looking for
//   - a section is capped at one 4096-byte flash page
//
// The store is NOT read through the XIP window. Access goes through a
// zpdb_read_fn supplied by the platform (see zpdb_flash.h, which streams
// the QSPI flash directly, bypassing the XIP cache and mapping), so a
// located object is a store-relative flash offset plus a byte length. The
// value is read straight into the destination the caller is going to serve
// from -- normally scsiDev.data -- so nothing is buffered on the way.

#pragma once

#include <stdint.h>
#include <stddef.h>

#define ZPDB_MAGIC              0x42445A50u  // 'Z','P','D','B' little-endian
#define ZPDB_VERSION            0x0100u      // 1.0, major in the high byte
#define ZPDB_VERSION_MAJOR(v)   ((uint8_t)((v) >> 8))
#define ZPDB_HEADER_SIZE        32u
#define ZPDB_SECTION_HDR_SIZE   8u
#define ZPDB_ENTRY_HDR_SIZE     12u
#define ZPDB_MAX_SECTION_SIZE   4096u        // one flash page
#define ZPDB_ALIGN              4u

// Longest section or key name that can be compared during a lookup. Names
// are read into a stack buffer this size; the builder warns above it.
#define ZPDB_MAX_NAME 64

// Value types. The numbering is part of the format.
enum zpdb_type_t : uint8_t
{
    ZPDB_TYPE_NULL = 0,  // key present, no value
    ZPDB_TYPE_LONG = 1,  // int32_t
    ZPDB_TYPE_U64  = 2,  // 64-bit integer, only 4-aligned -- read as two words
    ZPDB_TYPE_DATA = 3,  // raw binary (decoded hex string)
    ZPDB_TYPE_STR  = 4   // characters, val_len excludes the NUL terminator
};

struct zpdb_header_t
{
    uint32_t magic;         // ZPDB_MAGIC
    uint16_t version;       // ZPDB_VERSION
    uint16_t header_size;   // ZPDB_HEADER_SIZE; first section starts here
    uint32_t total_size;    // whole store including this header
    uint32_t section_count;
    uint32_t crc32;         // over [header_size, total_size)
    uint32_t build_epoch;   // build time, for staleness logging
    uint32_t flags;         // reserved, 0
    uint32_t reserved;      // reserved, 0
};

struct zpdb_section_hdr_t
{
    uint16_t sect_len;      // total bytes of the section, padding included
    uint8_t  entry_count;
    uint8_t  name_len;      // excludes the NUL
    uint32_t name_hash;     // FNV-1a-32 of the uppercased name
    // char name[]; NUL-terminated, zero-padded to 4
    // entries[entry_count];
};

struct zpdb_entry_hdr_t
{
    uint16_t entry_len;     // total bytes of the entry, padding included
    uint16_t val_off;       // offset from the entry start to the value
    uint32_t key_hash;      // FNV-1a-32 of the uppercased key
    uint16_t val_len;       // value length in bytes
    uint8_t  type;          // zpdb_type_t
    uint8_t  key_len;       // excludes the NUL
    // char key[]; NUL-terminated, zero-padded to 4
    // uint8_t value[val_len]; zero-padded to 4
};

static_assert(sizeof(zpdb_header_t) == ZPDB_HEADER_SIZE, "zpdb_header_t layout");
static_assert(sizeof(zpdb_section_hdr_t) == ZPDB_SECTION_HDR_SIZE, "zpdb_section_hdr_t layout");
static_assert(sizeof(zpdb_entry_hdr_t) == ZPDB_ENTRY_HDR_SIZE, "zpdb_entry_hdr_t layout");

// A located section: what a scan carries, without touching the store again.
struct zpdb_section_ref_t
{
    uint32_t offset;        // store-relative flash offset of the section
    uint32_t name_hash;
    uint16_t sect_len;
    uint8_t  entry_count;
    uint8_t  name_len;
};

// A located value: the flash offset and byte length asked for in the format.
struct zpdb_entry_ref_t
{
    uint32_t offset;        // store-relative flash offset of the entry
    uint32_t key_hash;
    uint16_t entry_len;
    uint16_t val_off;       // value is at offset + val_off
    uint16_t val_len;       // bytes of value
    uint8_t  type;          // zpdb_type_t
    uint8_t  key_len;

    uint32_t valueOffset() const { return offset + val_off; }
};

// Reads `len` bytes at store-relative `offset`. The implementation is
// responsible for any alignment bouncing the underlying flash path needs;
// callers here pass arbitrary offsets, lengths and destinations.
typedef bool (*zpdb_read_fn)(uint32_t offset, void *dest, uint32_t len, void *ctx);

// FNV-1a-32 over the ASCII-uppercased name. Must match fnv1a32() in
// utils/zpdb_build.py -- the stored hashes come from there.
uint32_t zpdb_hash(const char *name);
uint32_t zpdb_hash(const char *name, size_t len);

// CRC-32, zlib/reflected polynomial, for header verification.
uint32_t zpdb_crc32(const uint8_t *data, uint32_t len);

// Incremental form, so the writer can accumulate the CRC as bytes stream
// past instead of making an extra pass. Seed with ZPDB_CRC32_INIT and
// finish with ZPDB_CRC32_FINAL().
#define ZPDB_CRC32_INIT     0xFFFFFFFFu
#define ZPDB_CRC32_FINAL(c) (~(c))
uint32_t zpdb_crc32_update(uint32_t crc, const uint8_t *data, uint32_t len);

// A view over one ZPDB store. Holds the read callback and the validated
// header -- 48 bytes of state, no copy of the store itself.
class ZpdbStore
{
public:
    ZpdbStore() : m_read(nullptr), m_ctx(nullptr), m_open(false), m_size(0) {}

    // Validate and adopt the store the callback reads from. `region_size`
    // bounds it so a corrupt total_size cannot walk off the end of the
    // store's flash region. `verify_crc` streams the whole store past a small stack
    // buffer -- ~31 KB at boot today -- and is worth it there.
    //
    // Returns false and logs the reason on a bad magic, an unsupported major
    // version, an out-of-range size, or a CRC mismatch.
    bool open(zpdb_read_fn read, void *ctx, uint32_t region_size, bool verify_crc = true);

    void close() { m_open = false; }
    bool isOpen() const { return m_open; }
    uint32_t totalSize() const { return m_size; }
    uint32_t sectionCount() const { return m_header.section_count; }
    uint32_t buildEpoch() const { return m_header.build_epoch; }

    // --- section lookup -------------------------------------------------

    // Linear scan. Per section skipped: one 8-byte header read and an add.
    // The name is only read back for a hash hit. Case-insensitive.
    bool findSection(const char *name, zpdb_section_ref_t *out) const;

    bool firstSection(zpdb_section_ref_t *out) const;
    bool nextSection(zpdb_section_ref_t *ref) const;   // advances in place
    bool sectionName(const zpdb_section_ref_t &s, char *dest, uint32_t dest_size) const;

    // --- key lookup within a section ------------------------------------

    bool findKey(const zpdb_section_ref_t &s, const char *key, zpdb_entry_ref_t *out) const;

    bool firstEntry(const zpdb_section_ref_t &s, zpdb_entry_ref_t *out) const;
    bool nextEntry(const zpdb_section_ref_t &s, zpdb_entry_ref_t *ref) const;
    bool entryKey(const zpdb_entry_ref_t &e, char *dest, uint32_t dest_size) const;

    // Convenience for the VPD page tables: looks up "VPD%02X".
    bool findVPD(const zpdb_section_ref_t &s, uint8_t page_code, zpdb_entry_ref_t *out) const;

    // --- typed value access ---------------------------------------------

    // Reads a data or string value straight into `dest` -- normally
    // scsiDev.data, so the served bytes are never staged anywhere else.
    // Fails if the value is longer than dest_size. *out_len, when given,
    // gets the byte count actually read.
    bool readValue(const zpdb_entry_ref_t &e, void *dest, uint32_t dest_size,
                   uint32_t *out_len = nullptr) const;

    // As readValue(), but NUL-terminates and requires ZPDB_TYPE_STR.
    bool readString(const zpdb_entry_ref_t &e, char *dest, uint32_t dest_size) const;

    int32_t readLong(const zpdb_entry_ref_t &e, int32_t def = 0) const;

    // Reads the 8-byte payload as two 32-bit loads: the format guarantees
    // 4-byte alignment only. Accepts ZPDB_TYPE_LONG too, widening it.
    uint64_t readU64(const zpdb_entry_ref_t &e, uint64_t def = 0) const;

    // True when the key exists but carries no value, which is distinct from
    // the key being absent.
    static bool isNull(const zpdb_entry_ref_t &e) { return e.type == ZPDB_TYPE_NULL; }

    // Raw read at a store-relative offset, for callers that need it.
    bool read(uint32_t offset, void *dest, uint32_t len) const;

private:
    zpdb_read_fn m_read;
    void *m_ctx;
    bool m_open;
    uint32_t m_size;
    zpdb_header_t m_header;

    bool nameMatches(uint32_t offset, uint8_t len, const char *name) const;
};

// Writes a ZPDB store into flash using exactly three 4096-byte buffers:
// one to assemble the current section, one to stage the flash page being
// filled, and one holding page 0 back until the header is final. The header
// -- and with it the magic -- is programmed last, so a write interrupted
// part way reads back as an absent store, never as a valid truncated one.
//
// Callers drive it as: begin() -> for each section { beginSection(); addX();
// endSection(); } -> finish(). Any failure latches, so a caller may check
// only finish()'s result; the failing step is logged when it happens.
class ZpdbWriter
{
public:
    // Programs one 4096-byte page at store-relative `offset`. The region is
    // expected to have been erased before the write started.
    typedef bool (*program_page_t)(uint32_t offset, const uint8_t *page, void *ctx);

    ZpdbWriter(uint8_t *section_buf, uint8_t *page_buf, uint8_t *page0_buf)
        : m_section(section_buf), m_page(page_buf), m_page0(page0_buf) {}

    // `verify`, when given, is used to read each page back and compare it in
    // small chunks off the stack -- no fourth 4 KB buffer. `limit` is the
    // number of bytes available to write into, i.e. the size of the store's
    // flash region.
    void begin(program_page_t program, void *ctx, uint32_t limit,
               zpdb_read_fn verify = nullptr);

    bool beginSection(const char *name);
    bool addNull(const char *key);
    bool addLong(const char *key, int32_t value);
    bool addU64(const char *key, uint64_t value);
    bool addData(const char *key, const uint8_t *data, uint32_t len);
    bool addString(const char *key, const char *value);

    // Appends to the entry opened by the last addData() -- this is how
    // Key_0/Key_1/... chunks are concatenated without ever holding the
    // joined value twice.
    bool appendData(const uint8_t *data, uint32_t len);

    // Back-writes sect_len and flushes the section into the page stream.
    bool endSection();

    // Abandons the section being assembled, leaving everything already
    // committed intact. Used when one source file turns out to be bad.
    void abortSection();

    // Flushes the tail page, then programs page 0 with the finished header.
    bool finish();

    bool failed() const { return m_failed; }
    uint32_t sectionCount() const { return m_sections; }
    uint32_t bytesWritten() const { return m_total; }

private:
    uint8_t *m_section;
    uint8_t *m_page;
    uint8_t *m_page0;

    program_page_t m_program = nullptr;
    zpdb_read_fn m_verify = nullptr;
    void *m_ctx = nullptr;
    uint32_t m_limit = 0;

    uint32_t m_section_used = 0;   // bytes staged in m_section
    uint32_t m_last_entry = 0;     // offset of the entry open for appendData()
    bool m_in_section = false;
    uint32_t m_page_used = 0;      // bytes staged in m_page
    uint32_t m_page_base = 0;      // store offset of the page in m_page
    uint32_t m_total = ZPDB_HEADER_SIZE;
    uint32_t m_sections = 0;
    uint32_t m_crc = ZPDB_CRC32_INIT;
    bool m_failed = false;

    uint8_t *reserveEntry(const char *key, zpdb_type_t type, uint32_t val_len);
    bool emit(const uint8_t *data, uint32_t len);
    bool flushPage();
    bool verifyPage(uint32_t offset, const uint8_t *expected);
};
