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

#include "zpdb_flash.h"

#include "ZuluSCSI_log.h"
#include <ZuluSCSI_platform.h>

#include <string.h>

// Where the store lives is decided at build time by the flash map in
// ZuluSCSI_platform.h -- there is nothing to search for at runtime. All that
// is left to check is whether this build has a region at all.
#if defined(PLATFORM_HAS_FLASH_REGION_ACCESS) && defined(PLATFORM_FLASH_PROFILES_OFFSET)
# define ZPDB_HAVE_REGION 1

static const uint32_t ZPDB_BASE = PLATFORM_FLASH_PROFILES_OFFSET;
static const uint32_t ZPDB_SIZE = PLATFORM_FLASH_PROFILES_SIZE;

static_assert(PLATFORM_FLASH_PROFILES_SIZE >= ZPDB_MAX_SECTION_SIZE,
              "the profile store has to be able to hold at least one page");
static_assert((PLATFORM_FLASH_PROFILES_OFFSET % PLATFORM_FLASH_SECTOR_SIZE) == 0 &&
              (PLATFORM_FLASH_PROFILES_SIZE % PLATFORM_FLASH_SECTOR_SIZE) == 0,
              "the profile store has to start and end on an erase sector");
#else
static const uint32_t ZPDB_BASE = 0;
static const uint32_t ZPDB_SIZE = 0;
# ifndef PLATFORM_FLASH_SECTOR_SIZE
#  define PLATFORM_FLASH_SECTOR_SIZE 4096
# endif
#endif

static bool g_zpdb_ready = false;
static bool g_zpdb_checked = false;

bool zpdbFlashInit()
{
    if (g_zpdb_checked)
        return g_zpdb_ready;

    g_zpdb_checked = true;

#ifdef ZPDB_HAVE_REGION
    g_zpdb_ready = true;
    logmsg("---- ZPDB: profile store at flash offset ", (int)ZPDB_BASE,
           ", ", (int)(ZPDB_SIZE / 1024), " kB");
#else
    logmsg("---- ZPDB: this platform has no direct flash access, "
           "the profile store is unavailable");
#endif

    return g_zpdb_ready;
}

bool zpdbFlashAvailable()
{
    return g_zpdb_ready;
}

uint32_t zpdbFlashBase()
{
    return ZPDB_BASE;
}

uint32_t zpdbFlashSize()
{
    return ZPDB_SIZE;
}

bool zpdbFlashRead(uint32_t offset, void *dest, uint32_t len, void *ctx)
{
    (void)ctx;

    if (!g_zpdb_ready)
        return false;

    if (len > ZPDB_SIZE || offset > ZPDB_SIZE - len)
    {
        logmsg("---- ZPDB: read of ", (int)len, " bytes at ", (int)offset,
               " is outside the ", (int)ZPDB_SIZE, "-byte store");
        return false;
    }

#ifdef ZPDB_HAVE_REGION
    return platform_flash_read(ZPDB_BASE + offset, dest, len);
#else
    (void)dest;
    return false;
#endif
}

bool zpdbFlashErase()
{
    if (!g_zpdb_ready)
        return false;

#ifdef ZPDB_HAVE_REGION
    logmsg("---- ZPDB: erasing ", (int)(ZPDB_SIZE / 1024), " kB at flash offset ",
           (int)ZPDB_BASE);
    return platform_flash_erase(ZPDB_BASE, ZPDB_SIZE);
#else
    return false;
#endif
}

bool zpdbFlashProgramPage(uint32_t offset, const uint8_t *page, void *ctx)
{
    (void)ctx;

    if (!g_zpdb_ready)
        return false;

    if (offset > ZPDB_SIZE - ZPDB_MAX_SECTION_SIZE ||
        (offset % PLATFORM_FLASH_SECTOR_SIZE) != 0)
    {
        logmsg("---- ZPDB: page write at ", (int)offset, " is outside the ",
               (int)ZPDB_SIZE, "-byte store");
        return false;
    }

#ifdef ZPDB_HAVE_REGION
    return platform_flash_program(ZPDB_BASE + offset, page, ZPDB_MAX_SECTION_SIZE);
#else
    (void)page;
    return false;
#endif
}
