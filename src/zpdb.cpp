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

// ZPDB reader and flash writer. See docs/ZPDB_FORMAT.md.

#include "zpdb.h"

#include <string.h>

#ifdef ZPDB_HOST_TEST
// A host build (-DZPDB_HOST_TEST) has no firmware logging; keep the
// diagnostics, just route them somewhere printable.
# include <iostream>
# define ZPDB_LOG(...) zpdb_host_log(__VA_ARGS__)
template <typename... Params>
static void zpdb_host_log(Params... params)
{
    std::cout << "zpdb: ";
    (std::cout << ... << params) << std::endl;
}
#else
# include "ZuluSCSI_log.h"
# define ZPDB_LOG(...) dbgmsg("---- ZPDB: ", __VA_ARGS__)
#endif

// Chunk used to stream flash past the CRC and the page verifier. Small
// enough to sit on the stack of a boot-time call, big enough that the
// per-read overhead does not dominate.
#define ZPDB_STREAM_CHUNK 256

static inline uint32_t align4(uint32_t n)
{
    return (n + 3u) & ~3u;
}

static inline char upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - ('a' - 'A')) : c;
}

uint32_t zpdb_hash(const char *name, size_t len)
{
    uint32_t h = 0x811C9DC5u;
    for (size_t i = 0; i < len; i++)
    {
        h ^= (uint8_t)upper(name[i]);
        h *= 0x01000193u;
    }
    return h;
}

uint32_t zpdb_hash(const char *name)
{
    return zpdb_hash(name, strlen(name));
}

// Nibble-wise CRC-32 (reflected 0xEDB88320): 16 table entries instead of 256,
// which matters more here than the two-bits-per-step speed difference -- this
// runs once at boot and once per store rebuild.
uint32_t zpdb_crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    static const uint32_t table[16] = {
        0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
        0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
        0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
        0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C
    };

    for (uint32_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        crc = (crc >> 4) ^ table[crc & 0x0F];
        crc = (crc >> 4) ^ table[crc & 0x0F];
    }
    return crc;
}

uint32_t zpdb_crc32(const uint8_t *data, uint32_t len)
{
    return ZPDB_CRC32_FINAL(zpdb_crc32_update(ZPDB_CRC32_INIT, data, len));
}

// ---------------------------------------------------------------- ZpdbStore

bool ZpdbStore::read(uint32_t offset, void *dest, uint32_t len) const
{
    if (m_read == nullptr) return false;
    return m_read(offset, dest, len, m_ctx);
}

bool ZpdbStore::open(zpdb_read_fn read, void *ctx, uint32_t region_size, bool verify_crc)
{
    m_open = false;
    m_read = read;
    m_ctx = ctx;
    m_size = 0;

    if (read == nullptr || region_size < ZPDB_HEADER_SIZE)
    {
        ZPDB_LOG("store region is too small: ", (int)region_size, " bytes");
        return false;
    }

    if (!read(0, &m_header, sizeof(m_header), ctx))
    {
        ZPDB_LOG("could not read the store header");
        return false;
    }

    if (m_header.magic != ZPDB_MAGIC)
    {
        // Expected when the region has never been written; the caller decides
        // whether that is an error or a cue to build the store.
        ZPDB_LOG("no store found (magic 0x", (uint32_t)m_header.magic, ")");
        return false;
    }

    if (ZPDB_VERSION_MAJOR(m_header.version) != ZPDB_VERSION_MAJOR(ZPDB_VERSION))
    {
        ZPDB_LOG("unsupported format version 0x", (uint16_t)m_header.version,
                 ", this build understands 0x", (uint16_t)ZPDB_VERSION);
        return false;
    }

    if (m_header.header_size < ZPDB_HEADER_SIZE || (m_header.header_size & 3u) != 0 ||
        m_header.total_size < m_header.header_size ||
        m_header.total_size > region_size || (m_header.total_size & 3u) != 0)
    {
        ZPDB_LOG("header is inconsistent: total_size ", (int)m_header.total_size,
                 ", region ", (int)region_size);
        return false;
    }

    if (verify_crc)
    {
        uint8_t chunk[ZPDB_STREAM_CHUNK];
        uint32_t crc = ZPDB_CRC32_INIT;
        uint32_t pos = m_header.header_size;

        while (pos < m_header.total_size)
        {
            uint32_t n = m_header.total_size - pos;
            if (n > sizeof(chunk)) n = sizeof(chunk);

            if (!read(pos, chunk, n, ctx))
            {
                ZPDB_LOG("could not read the store at offset ", (int)pos);
                return false;
            }
            crc = zpdb_crc32_update(crc, chunk, n);
            pos += n;
        }

        crc = ZPDB_CRC32_FINAL(crc);
        if (crc != m_header.crc32)
        {
            ZPDB_LOG("CRC mismatch: computed 0x", (uint32_t)crc,
                     ", stored 0x", (uint32_t)m_header.crc32);
            return false;
        }
    }

    m_size = m_header.total_size;
    m_open = true;
    return true;
}

bool ZpdbStore::firstSection(zpdb_section_ref_t *out) const
{
    if (!m_open || m_header.section_count == 0)
        return false;

    uint32_t offset = m_header.header_size;
    if (offset + ZPDB_SECTION_HDR_SIZE > m_size)
        return false;

    zpdb_section_hdr_t hdr;
    if (!read(offset, &hdr, sizeof(hdr)))
        return false;

    if (hdr.sect_len < ZPDB_SECTION_HDR_SIZE || (hdr.sect_len & 3u) != 0 ||
        hdr.sect_len > ZPDB_MAX_SECTION_SIZE || offset + hdr.sect_len > m_size)
    {
        ZPDB_LOG("section at ", (int)offset, " has a bad length ", (int)hdr.sect_len);
        return false;
    }

    out->offset = offset;
    out->sect_len = hdr.sect_len;
    out->name_hash = hdr.name_hash;
    out->entry_count = hdr.entry_count;
    out->name_len = hdr.name_len;
    return true;
}

bool ZpdbStore::nextSection(zpdb_section_ref_t *ref) const
{
    if (!m_open) return false;

    // The one field a scan has to touch: skip the whole section, keys and
    // all, by reading its 8-byte header and adding its own length.
    uint32_t offset = ref->offset + ref->sect_len;
    if (offset + ZPDB_SECTION_HDR_SIZE > m_size)
        return false;

    zpdb_section_hdr_t hdr;
    if (!read(offset, &hdr, sizeof(hdr)))
        return false;

    if (hdr.sect_len < ZPDB_SECTION_HDR_SIZE || (hdr.sect_len & 3u) != 0 ||
        hdr.sect_len > ZPDB_MAX_SECTION_SIZE || offset + hdr.sect_len > m_size)
    {
        ZPDB_LOG("section at ", (int)offset, " has a bad length ", (int)hdr.sect_len);
        return false;
    }

    ref->offset = offset;
    ref->sect_len = hdr.sect_len;
    ref->name_hash = hdr.name_hash;
    ref->entry_count = hdr.entry_count;
    ref->name_len = hdr.name_len;
    return true;
}

bool ZpdbStore::nameMatches(uint32_t offset, uint8_t len, const char *name) const
{
    if (len >= ZPDB_MAX_NAME)
        return false;   // longer than anything this build can compare

    char stored[ZPDB_MAX_NAME];
    if (!read(offset, stored, len))
        return false;

    for (uint8_t i = 0; i < len; i++)
    {
        if (name[i] == '\0' || upper(stored[i]) != upper(name[i]))
            return false;
    }
    return name[len] == '\0';
}

bool ZpdbStore::sectionName(const zpdb_section_ref_t &s, char *dest, uint32_t dest_size) const
{
    if (dest_size <= s.name_len) return false;
    if (!read(s.offset + ZPDB_SECTION_HDR_SIZE, dest, s.name_len)) return false;
    dest[s.name_len] = '\0';
    return true;
}

bool ZpdbStore::findSection(const char *name, zpdb_section_ref_t *out) const
{
    if (name == nullptr || !m_open)
        return false;

    const uint32_t want = zpdb_hash(name);

    zpdb_section_ref_t s;
    for (bool ok = firstSection(&s); ok; ok = nextSection(&s))
    {
        // The 8-byte header read by the scan already carried the hash, so a
        // miss costs nothing beyond that read and an add.
        if (s.name_hash == want &&
            nameMatches(s.offset + ZPDB_SECTION_HDR_SIZE, s.name_len, name))
        {
            *out = s;
            return true;
        }
    }
    return false;
}

bool ZpdbStore::firstEntry(const zpdb_section_ref_t &s, zpdb_entry_ref_t *out) const
{
    if (!m_open || s.entry_count == 0)
        return false;

    uint32_t off = ZPDB_SECTION_HDR_SIZE + align4((uint32_t)s.name_len + 1u);
    if (off + ZPDB_ENTRY_HDR_SIZE > s.sect_len)
        return false;

    zpdb_entry_hdr_t hdr;
    if (!read(s.offset + off, &hdr, sizeof(hdr)))
        return false;

    if (hdr.entry_len < ZPDB_ENTRY_HDR_SIZE || (hdr.entry_len & 3u) != 0 ||
        off + hdr.entry_len > s.sect_len ||
        (uint32_t)hdr.val_off + hdr.val_len > hdr.entry_len)
    {
        ZPDB_LOG("entry at ", (int)(s.offset + off), " is malformed");
        return false;
    }

    out->offset = s.offset + off;
    out->entry_len = hdr.entry_len;
    out->key_hash = hdr.key_hash;
    out->val_off = hdr.val_off;
    out->val_len = hdr.val_len;
    out->type = hdr.type;
    out->key_len = hdr.key_len;
    return true;
}

bool ZpdbStore::nextEntry(const zpdb_section_ref_t &s, zpdb_entry_ref_t *ref) const
{
    if (!m_open) return false;

    uint32_t offset = ref->offset + ref->entry_len;
    uint32_t rel = offset - s.offset;
    if (rel + ZPDB_ENTRY_HDR_SIZE > s.sect_len)
        return false;

    zpdb_entry_hdr_t hdr;
    if (!read(offset, &hdr, sizeof(hdr)))
        return false;

    if (hdr.entry_len < ZPDB_ENTRY_HDR_SIZE || (hdr.entry_len & 3u) != 0 ||
        rel + hdr.entry_len > s.sect_len ||
        (uint32_t)hdr.val_off + hdr.val_len > hdr.entry_len)
    {
        ZPDB_LOG("entry at ", (int)offset, " is malformed");
        return false;
    }

    ref->offset = offset;
    ref->entry_len = hdr.entry_len;
    ref->key_hash = hdr.key_hash;
    ref->val_off = hdr.val_off;
    ref->val_len = hdr.val_len;
    ref->type = hdr.type;
    ref->key_len = hdr.key_len;
    return true;
}

bool ZpdbStore::entryKey(const zpdb_entry_ref_t &e, char *dest, uint32_t dest_size) const
{
    if (dest_size <= e.key_len) return false;
    if (!read(e.offset + ZPDB_ENTRY_HDR_SIZE, dest, e.key_len)) return false;
    dest[e.key_len] = '\0';
    return true;
}

bool ZpdbStore::findKey(const zpdb_section_ref_t &s, const char *key,
                        zpdb_entry_ref_t *out) const
{
    if (key == nullptr || !m_open)
        return false;

    const uint32_t want = zpdb_hash(key);

    zpdb_entry_ref_t e;
    for (bool ok = firstEntry(s, &e); ok; ok = nextEntry(s, &e))
    {
        if (e.key_hash == want &&
            nameMatches(e.offset + ZPDB_ENTRY_HDR_SIZE, e.key_len, key))
        {
            *out = e;
            return true;
        }
    }
    return false;
}

bool ZpdbStore::findVPD(const zpdb_section_ref_t &s, uint8_t page_code,
                        zpdb_entry_ref_t *out) const
{
    static const char hex[] = "0123456789ABCDEF";
    char key[6] = { 'V', 'P', 'D', hex[page_code >> 4], hex[page_code & 0x0F], '\0' };
    return findKey(s, key, out);
}

bool ZpdbStore::readValue(const zpdb_entry_ref_t &e, void *dest, uint32_t dest_size,
                          uint32_t *out_len) const
{
    if (e.type != ZPDB_TYPE_DATA && e.type != ZPDB_TYPE_STR)
        return false;

    if (e.val_len > dest_size)
    {
        ZPDB_LOG("value of ", (int)e.val_len, " bytes does not fit a ",
                 (int)dest_size, "-byte destination");
        return false;
    }

    if (!read(e.valueOffset(), dest, e.val_len))
        return false;

    if (out_len != nullptr) *out_len = e.val_len;
    return true;
}

bool ZpdbStore::readString(const zpdb_entry_ref_t &e, char *dest, uint32_t dest_size) const
{
    if (e.type != ZPDB_TYPE_STR || dest_size <= e.val_len)
        return false;

    if (!read(e.valueOffset(), dest, e.val_len))
        return false;

    dest[e.val_len] = '\0';
    return true;
}

int32_t ZpdbStore::readLong(const zpdb_entry_ref_t &e, int32_t def) const
{
    if (e.type == ZPDB_TYPE_LONG && e.val_len >= 4)
    {
        int32_t v;
        if (!read(e.valueOffset(), &v, sizeof(v))) return def;
        return v;
    }
    if (e.type == ZPDB_TYPE_U64)
        return (int32_t)readU64(e, (uint64_t)def);

    return def;
}

uint64_t ZpdbStore::readU64(const zpdb_entry_ref_t &e, uint64_t def) const
{
    if (e.type == ZPDB_TYPE_LONG && e.val_len >= 4)
        return (uint64_t)(int64_t)readLong(e, (int32_t)def);

    if (e.type != ZPDB_TYPE_U64 || e.val_len < 8)
        return def;

    // The format only promises 4-byte alignment, and a 64-bit load off a
    // 4-aligned address is not safe on the M33 -- take it as two words.
    uint32_t words[2];
    if (!read(e.valueOffset(), words, sizeof(words))) return def;
    return ((uint64_t)words[1] << 32) | words[0];
}

// --------------------------------------------------------------- ZpdbWriter

void ZpdbWriter::begin(program_page_t program, void *ctx, uint32_t limit,
                       zpdb_read_fn verify)
{
    m_program = program;
    m_ctx = ctx;
    m_verify = verify;
    m_limit = limit;

    m_section_used = 0;
    m_last_entry = 0;
    m_in_section = false;
    m_page_used = ZPDB_HEADER_SIZE;   // page 0 opens with the header's hole
    m_page_base = 0;
    m_total = ZPDB_HEADER_SIZE;
    m_sections = 0;
    m_crc = ZPDB_CRC32_INIT;
    m_failed = (program == nullptr || limit < ZPDB_MAX_SECTION_SIZE);

    if (m_program == nullptr)
        ZPDB_LOG("writer started without a page program callback");
    else if (m_failed)
        ZPDB_LOG("write region of ", (int)limit, " bytes is smaller than one page");
    else
        memset(m_page0, 0, ZPDB_MAX_SECTION_SIZE);
}

bool ZpdbWriter::verifyPage(uint32_t offset, const uint8_t *expected)
{
    if (m_verify == nullptr)
        return true;

    uint8_t chunk[ZPDB_STREAM_CHUNK];
    for (uint32_t pos = 0; pos < ZPDB_MAX_SECTION_SIZE; pos += sizeof(chunk))
    {
        if (!m_verify(offset + pos, chunk, sizeof(chunk), m_ctx) ||
            memcmp(chunk, expected + pos, sizeof(chunk)) != 0)
        {
            return false;
        }
    }
    return true;
}

bool ZpdbWriter::flushPage()
{
    // Page 0 is held in its own buffer until finish(): the header is only
    // complete once every section has been written, and programming the
    // magic last is what keeps an interrupted write from looking valid.
    if (m_page_base != 0)
    {
        if (!m_program(m_page_base, m_page, m_ctx))
        {
            ZPDB_LOG("failed to program page at offset ", (int)m_page_base);
            m_failed = true;
            return false;
        }

        if (!verifyPage(m_page_base, m_page))
        {
            ZPDB_LOG("verify failed for page at offset ", (int)m_page_base);
            m_failed = true;
            return false;
        }
    }

    m_page_base += ZPDB_MAX_SECTION_SIZE;
    m_page_used = 0;
    memset(m_page, 0, ZPDB_MAX_SECTION_SIZE);
    return true;
}

bool ZpdbWriter::emit(const uint8_t *data, uint32_t len)
{
    while (len > 0 && !m_failed)
    {
        if (m_page_base + m_page_used >= m_limit)
        {
            ZPDB_LOG("store does not fit the ", (int)m_limit, "-byte write region");
            m_failed = true;
            return false;
        }

        uint8_t *cur = (m_page_base == 0) ? m_page0 : m_page;
        uint32_t room = ZPDB_MAX_SECTION_SIZE - m_page_used;
        uint32_t n = (len < room) ? len : room;

        memcpy(cur + m_page_used, data, n);
        m_crc = zpdb_crc32_update(m_crc, data, n);
        m_page_used += n;
        data += n;
        len -= n;

        if (m_page_used == ZPDB_MAX_SECTION_SIZE && !flushPage())
            return false;
    }
    return !m_failed;
}

bool ZpdbWriter::beginSection(const char *name)
{
    if (m_failed) return false;

    if (m_in_section)
    {
        ZPDB_LOG("beginSection() called with a section still open");
        m_failed = true;
        return false;
    }

    size_t name_len = strlen(name);
    uint32_t name_field = align4((uint32_t)name_len + 1u);

    if (name_len >= ZPDB_MAX_NAME)
    {
        // A name this build could not compare back at lookup time is worse
        // than no section at all, so refuse it here.
        ZPDB_LOG("section name '", name, "' is longer than ", (int)(ZPDB_MAX_NAME - 1),
                 " characters");
        return false;
    }

    memset(m_section, 0, ZPDB_SECTION_HDR_SIZE + name_field);

    zpdb_section_hdr_t *s = (zpdb_section_hdr_t *)m_section;
    s->sect_len = 0;                    // back-written by endSection()
    s->entry_count = 0;
    s->name_len = (uint8_t)name_len;
    s->name_hash = zpdb_hash(name, name_len);
    memcpy(m_section + ZPDB_SECTION_HDR_SIZE, name, name_len);

    m_section_used = ZPDB_SECTION_HDR_SIZE + name_field;
    m_last_entry = 0;
    m_in_section = true;
    return true;
}

void ZpdbWriter::abortSection()
{
    m_in_section = false;
    m_section_used = 0;
    m_last_entry = 0;
}

uint8_t *ZpdbWriter::reserveEntry(const char *key, zpdb_type_t type, uint32_t val_len)
{
    if (m_failed) return nullptr;

    if (!m_in_section)
    {
        ZPDB_LOG("entry '", key, "' added with no section open");
        m_failed = true;
        return nullptr;
    }

    zpdb_section_hdr_t *s = (zpdb_section_hdr_t *)m_section;
    if (s->entry_count == 255)
    {
        ZPDB_LOG("section has hit the 255-entry limit");
        return nullptr;
    }

    size_t key_len = strlen(key);
    if (key_len >= ZPDB_MAX_NAME)
    {
        ZPDB_LOG("key '", key, "' is longer than ", (int)(ZPDB_MAX_NAME - 1), " characters");
        return nullptr;
    }

    uint32_t key_field = align4((uint32_t)key_len + 1u);
    // A string keeps at least one NUL of padding so a reader can pull it out
    // as a C string; other types just round up.
    uint32_t val_field = (type == ZPDB_TYPE_STR) ? align4(val_len + 1u) : align4(val_len);
    uint32_t val_off = ZPDB_ENTRY_HDR_SIZE + key_field;
    uint32_t entry_len = val_off + val_field;

    if (m_section_used + entry_len > ZPDB_MAX_SECTION_SIZE)
    {
        // This is the whole reason the section buffer is exactly one page:
        // an oversized section is caught here, before any flash is touched.
        ZPDB_LOG("section would exceed the ", (int)ZPDB_MAX_SECTION_SIZE,
                 "-byte limit at ", (int)(m_section_used + entry_len), " bytes");
        return nullptr;
    }

    uint8_t *base = m_section + m_section_used;
    memset(base, 0, entry_len);

    zpdb_entry_hdr_t *e = (zpdb_entry_hdr_t *)base;
    e->entry_len = (uint16_t)entry_len;
    e->val_off = (uint16_t)val_off;
    e->key_hash = zpdb_hash(key, key_len);
    e->val_len = (uint16_t)val_len;
    e->type = (uint8_t)type;
    e->key_len = (uint8_t)key_len;
    memcpy(base + ZPDB_ENTRY_HDR_SIZE, key, key_len);

    m_last_entry = m_section_used;
    m_section_used += entry_len;
    s->entry_count++;

    return base + val_off;
}

bool ZpdbWriter::addNull(const char *key)
{
    return reserveEntry(key, ZPDB_TYPE_NULL, 0) != nullptr;
}

bool ZpdbWriter::addLong(const char *key, int32_t value)
{
    uint8_t *v = reserveEntry(key, ZPDB_TYPE_LONG, 4);
    if (v == nullptr) return false;
    memcpy(v, &value, sizeof(value));
    return true;
}

bool ZpdbWriter::addU64(const char *key, uint64_t value)
{
    uint8_t *v = reserveEntry(key, ZPDB_TYPE_U64, 8);
    if (v == nullptr) return false;
    uint32_t lo = (uint32_t)value, hi = (uint32_t)(value >> 32);
    memcpy(v, &lo, sizeof(lo));
    memcpy(v + 4, &hi, sizeof(hi));
    return true;
}

bool ZpdbWriter::addData(const char *key, const uint8_t *data, uint32_t len)
{
    if (len > ZPDB_MAX_SECTION_SIZE)
    {
        ZPDB_LOG("data value of ", (int)len, " bytes cannot fit a section");
        return false;
    }

    uint8_t *v = reserveEntry(key, ZPDB_TYPE_DATA, len);
    if (v == nullptr) return false;
    if (len > 0) memcpy(v, data, len);
    return true;
}

bool ZpdbWriter::addString(const char *key, const char *value)
{
    uint32_t len = (uint32_t)strlen(value);
    uint8_t *v = reserveEntry(key, ZPDB_TYPE_STR, len);
    if (v == nullptr) return false;
    memcpy(v, value, len);   // the NUL is already there: reserveEntry zeroed it
    return true;
}

bool ZpdbWriter::appendData(const uint8_t *data, uint32_t len)
{
    if (m_failed) return false;

    if (!m_in_section || m_last_entry == 0)
    {
        ZPDB_LOG("appendData() with no data entry open");
        return false;
    }

    zpdb_entry_hdr_t *e = (zpdb_entry_hdr_t *)(m_section + m_last_entry);
    if (e->type != ZPDB_TYPE_DATA)
    {
        ZPDB_LOG("appendData() on an entry that is not a data array");
        return false;
    }

    uint32_t val_len = (uint32_t)e->val_len + len;
    uint32_t entry_len = (uint32_t)e->val_off + align4(val_len);

    if (m_last_entry + entry_len > ZPDB_MAX_SECTION_SIZE)
    {
        ZPDB_LOG("chunk append would exceed the ", (int)ZPDB_MAX_SECTION_SIZE,
                 "-byte section limit at ", (int)(m_last_entry + entry_len), " bytes");
        return false;
    }

    uint8_t *dst = m_section + m_last_entry + e->val_off + e->val_len;
    memcpy(dst, data, len);

    // Re-zero the padding the previous length rounded up to.
    uint32_t used = (uint32_t)e->val_off + val_len;
    memset(m_section + m_last_entry + used, 0, entry_len - used);

    e->val_len = (uint16_t)val_len;
    e->entry_len = (uint16_t)entry_len;
    m_section_used = m_last_entry + entry_len;
    return true;
}

bool ZpdbWriter::endSection()
{
    if (m_failed) return false;

    if (!m_in_section)
    {
        ZPDB_LOG("endSection() with no section open");
        m_failed = true;
        return false;
    }

    zpdb_section_hdr_t *s = (zpdb_section_hdr_t *)m_section;
    s->sect_len = (uint16_t)m_section_used;   // the back-written length

    m_total += m_section_used;
    m_sections++;
    m_in_section = false;

    return emit(m_section, m_section_used);
}

bool ZpdbWriter::finish()
{
    if (m_in_section)
    {
        ZPDB_LOG("finish() with a section still open");
        m_failed = true;
    }
    if (m_failed) return false;

    // Flush the tail page (zero-padded), unless everything still lives in
    // the held-back page 0.
    if (m_page_base != 0 && m_page_used > 0)
    {
        memset(m_page + m_page_used, 0, ZPDB_MAX_SECTION_SIZE - m_page_used);
        if (!flushPage()) return false;
    }

    zpdb_header_t *h = (zpdb_header_t *)m_page0;
    h->magic = ZPDB_MAGIC;
    h->version = ZPDB_VERSION;
    h->header_size = ZPDB_HEADER_SIZE;
    h->total_size = m_total;
    h->section_count = m_sections;
    h->build_epoch = 0;
    h->flags = 0;
    h->reserved = 0;

    // The CRC was accumulated in emit() as the section bytes streamed past,
    // so finishing it costs nothing and needs no second copy of the data.
    h->crc32 = ZPDB_CRC32_FINAL(m_crc);

    if (!m_program(0, m_page0, m_ctx))
    {
        ZPDB_LOG("failed to program the header page");
        m_failed = true;
        return false;
    }

    if (!verifyPage(0, m_page0))
    {
        ZPDB_LOG("verify failed for the header page");
        m_failed = true;
        return false;
    }

    return true;
}
