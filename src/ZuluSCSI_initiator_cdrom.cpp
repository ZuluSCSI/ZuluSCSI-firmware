/**
 * ZuluSCSI™ - Copyright (c) 2026 Rabbit Hole Computing™
 *
 * ZuluSCSI™ firmware is licensed under the GPL version 3 or any later version.
 *
 * https://www.gnu.org/licenses/gpl-3.0.html
 * ----
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

/*
 * Initiator mode imaging of CD-ROM drives into .bin/.cue files.
 *
 * Discs with a single data track are imaged as .iso by the main initiator
 * code. Discs with audio tracks or several tracks are imaged here, track by
 * track according to the table of contents, into a single .bin file with a
 * .cue sheet that the ZuluSCSI CD-ROM emulation can mount.
 *
 * Audio tracks are read as 2352 byte sectors. Drives differ in how they
 * return digital audio, the first of these methods that works is used:
 *   - READ CD (0xBE), MMC drives
 *   - READ CDDA (0xD8), Plextor, Sony and others
 *   - READ CDDA (0xD4), NEC
 *   - READ(10) after selecting density code 0x82, Toshiba and others
 *   - READ(10) after selecting a 2352 byte block size
 *
 * Data tracks are read raw (2352 bytes, MODE1/2352 or MODE2/2352) with
 * READ CD when the drive supports it, otherwise as 2048 byte user data
 * (MODE1/2048) with READ(10).
 *
 * Areas that cannot be read at the end of a track, such as the gap between
 * the sessions of a CD-Extra disc, are left out of the .bin file and
 * recorded as PREGAP of the next track, so that all tracks keep their
 * position on the disc.
 */

#include "ZuluSCSI_initiator_cdrom.h"
#include "ZuluSCSI_initiator.h"
#include "ZuluSCSI_config.h"
#include "ZuluSCSI_log.h"
#include <ZuluSCSI_platform.h>
#include <minIni.h>
#include "SdFat.h"

#include <scsi2sd.h>
extern "C" {
#include <scsi.h>
}

#ifdef PLATFORM_HAS_INITIATOR_MODE

extern SdFs SD;

#define CD_RAW_SECTOR  2352
#define CD_DATA_SECTOR 2048
#define CD_MAX_TRACKS  99
#define CD_LEADOUT_TRACK 0xAA

// Gap between the sessions of a multisession disc: lead-out of the
// previous session, lead-in of the next and the pregap of its first track.
// The lead-out of the first session is 6750 sectors, later ones 2250.
#define CD_SESSION_GAP_FIRST (6750 + 4500 + 150)
#define CD_SESSION_GAP_LATER (2250 + 4500 + 150)

// After this many consecutive unreadable sectors near the end of a track,
// check whether the rest of the track is a gap that cannot be read.
#define CD_BAD_RUN_GAP_CHECK 16
#define CD_MAX_GAP (CD_SESSION_GAP_FIRST + 150)

enum cd_read_method_t {
    CD_READ_NONE,
    CD_READ_READCD,     // READ CD (0xBE)
    CD_READ_D8,         // READ CDDA (0xD8)
    CD_READ_D4_NEC,     // NEC READ CDDA (0xD4)
    CD_READ_10_D82,     // READ(10) with density code 0x82, 2352 bytes
    CD_READ_10_2352,    // READ(10) with 2352 byte block size
    CD_READ_10_2048,    // READ(10) with 2048 byte block size
};

static const char *cdMethodName(cd_read_method_t method)
{
    switch (method)
    {
        case CD_READ_READCD:  return "READ CD";
        case CD_READ_D8:      return "READ CDDA (D8)";
        case CD_READ_D4_NEC:  return "NEC READ CDDA (D4)";
        case CD_READ_10_D82:  return "READ(10) density 0x82";
        case CD_READ_10_2352: return "READ(10) 2352 byte blocks";
        case CD_READ_10_2048: return "READ(10) 2048 byte blocks";
        default:              return "none";
    }
}

struct cd_track_t {
    uint8_t number;
    uint8_t control;    // Q subchannel control field from TOC
    uint8_t data_mode;  // Data track mode from sector header, 1 or 2
    uint32_t start;     // Start LBA from TOC
    uint32_t end;       // LBA where reading stops (exclusive)
    uint32_t read_end;  // LBA where reading actually stopped
    uint32_t file_frame; // Number of sectors stored in .bin before this track
};

static struct {
    int target_id;
    FsFile *file;
    char bin_filename[64];
    uint8_t max_retry;

    cd_track_t tracks[CD_MAX_TRACKS];
    int track_count;
    uint32_t leadout;

    cd_read_method_t audio_method;
    cd_read_method_t data_method;

    // Block size and density selected on the drive, -1 if unknown
    int cur_density;
    int32_t cur_blocklen;
    bool mode_changed;

    // Imaging progress
    int cur_track;
    bool track_started;
    uint32_t lba;
    uint32_t stored_frames;
    uint32_t sectors_done;
    uint32_t total_sectors;
    uint64_t total_bytes;

    int retrycount;
    uint32_t failposition;
    uint32_t bad_run_len;
    uint32_t bad_run_start;
    uint64_t bad_run_filepos;

    uint32_t bad_sectors;
    uint32_t skipped_sectors;
    uint32_t start_time;
} g_cd;

static inline bool cdIsAudio(const cd_track_t &t)
{
    return (t.control & 0x04) == 0;
}

static uint32_t readBE32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void writeBE32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

static cd_read_method_t cdTrackMethod(const cd_track_t &t)
{
    return cdIsAudio(t) ? g_cd.audio_method : g_cd.data_method;
}

static uint32_t cdSectorSize(cd_read_method_t method)
{
    return (method == CD_READ_10_2048) ? CD_DATA_SECTOR : CD_RAW_SECTOR;
}

/*************************************
 * Drive commands                    *
 *************************************/

// Select density code and block size with MODE SELECT(6)
static bool cdSetBlockMode(int density, uint32_t blocklen)
{
    if (g_cd.cur_density == density && g_cd.cur_blocklen == (int32_t)blocklen)
    {
        return true;
    }

    uint8_t params[12] = {0, 0, 0, 8,
        (uint8_t)density, 0, 0, 0,
        0, (uint8_t)(blocklen >> 16), (uint8_t)(blocklen >> 8), (uint8_t)blocklen
    };

    // Drives differ in whether they want the page format bit set
    for (uint8_t pf = 0x10; ; pf = 0)
    {
        uint8_t command[6] = {0x15, pf, 0, 0, sizeof(params), 0};
        int status = scsiInitiatorRunCommand(g_cd.target_id, command, sizeof(command),
                                             NULL, 0, params, sizeof(params));
        if (status == 0)
        {
            dbgmsg("-- CD block mode set to density ", (uint8_t)density, " size ", (int)blocklen);
            g_cd.cur_density = density;
            g_cd.cur_blocklen = blocklen;
            g_cd.mode_changed = true;
            return true;
        }

        if (status == 2)
        {
            uint8_t sense_key;
            scsiRequestSense(g_cd.target_id, &sense_key);
        }

        if (pf == 0) break;
    }

    g_cd.cur_density = -1;
    g_cd.cur_blocklen = -1;
    return false;
}

// Read sectors with the given method. Returns SCSI status, 0 on success.
static int cdReadSectors(cd_read_method_t method, bool audio, uint32_t lba, uint32_t count, uint8_t *buf)
{
    uint8_t command[12] = {0};
    size_t cmdlen = 10;

    switch (method)
    {
        case CD_READ_READCD:
            command[0] = 0xBE;
            command[1] = audio ? (1 << 2) : 0; // Expected sector type CD-DA or any
            writeBE32(&command[2], lba);
            command[6] = count >> 16;
            command[7] = count >> 8;
            command[8] = count;
            command[9] = audio ? 0x10 : 0xF8;  // User data, or sync + headers + data + EDC/ECC
            cmdlen = 12;
            break;

        case CD_READ_D8:
            command[0] = 0xD8;
            writeBE32(&command[2], lba);
            writeBE32(&command[6], count);
            cmdlen = 12;
            break;

        case CD_READ_D4_NEC:
            command[0] = 0xD4;
            writeBE32(&command[2], lba);
            command[7] = count >> 8;
            command[8] = count;
            break;

        case CD_READ_10_D82:
        case CD_READ_10_2352:
        case CD_READ_10_2048:
        {
            int density = (method == CD_READ_10_D82) ? 0x82 : 0x00;
            if (!cdSetBlockMode(density, cdSectorSize(method)))
            {
                return -4;
            }
            command[0] = 0x28;
            writeBE32(&command[2], lba);
            command[7] = count >> 8;
            command[8] = count;
            break;
        }

        default:
            return -4;
    }

    uint32_t len = count * cdSectorSize(method);
    int status = -1;

    // A drive reports UNIT ATTENTION once after MODE SELECT, repeat the command then
    for (int attempt = 0; attempt < 2; attempt++)
    {
        status = scsiInitiatorRunCommand(g_cd.target_id, command, cmdlen, buf, len, NULL, 0);
        if (status != 2)
        {
            break;
        }

        uint8_t sense_key = 0, asc = 0, ascq = 0;
        scsiRequestSense(g_cd.target_id, &sense_key, &asc, &ascq);
        if (sense_key == RECOVERED_ERROR)
        {
            return 0;
        }
        dbgmsg("-- CD read at LBA ", (int)lba, " with ", cdMethodName(method), " failed, sense key ",
               sense_key, " asc ", asc, " ascq ", ascq);
        if (sense_key != UNIT_ATTENTION)
        {
            break;
        }
    }

    return status;
}

// Read the table of contents
static bool cdReadTOC()
{
    uint8_t *buf = scsiDev.data;

    // Starting track 0 returns all tracks on MMC drives,
    // some older drives want the number of the first track.
    for (uint8_t start_track = 0; start_track <= 1; start_track++)
    {
        uint8_t header_cmd[10] = {0x43, 0, 0, 0, 0, 0, start_track, 0, 4, 0};
        int status = scsiInitiatorRunCommand(g_cd.target_id, header_cmd, sizeof(header_cmd),
                                             buf, 4, NULL, 0);
        if (status != 0)
        {
            if (status == 2)
            {
                uint8_t sense_key;
                scsiRequestSense(g_cd.target_id, &sense_key);
            }
            continue;
        }

        uint32_t len = (((uint32_t)buf[0] << 8) | buf[1]) + 2;
        if (len < 12) continue;
        if (len > 4 + 8 * (CD_MAX_TRACKS + 1)) len = 4 + 8 * (CD_MAX_TRACKS + 1);
        len = 4 + (len - 4) / 8 * 8;

        uint8_t toc_cmd[10] = {0x43, 0, 0, 0, 0, 0, start_track, (uint8_t)(len >> 8), (uint8_t)len, 0};
        status = scsiInitiatorRunCommand(g_cd.target_id, toc_cmd, sizeof(toc_cmd),
                                         buf, len, NULL, 0);
        if (status != 0)
        {
            if (status == 2)
            {
                uint8_t sense_key;
                scsiRequestSense(g_cd.target_id, &sense_key);
            }
            continue;
        }

        g_cd.track_count = 0;
        g_cd.leadout = 0;
        for (uint32_t pos = 4; pos + 8 <= len; pos += 8)
        {
            uint8_t *desc = &buf[pos];
            uint8_t number = desc[2];
            uint32_t lba = readBE32(&desc[4]);

            if (number == CD_LEADOUT_TRACK)
            {
                g_cd.leadout = lba;
            }
            else if (number >= 1 && number <= CD_MAX_TRACKS && g_cd.track_count < CD_MAX_TRACKS)
            {
                cd_track_t &t = g_cd.tracks[g_cd.track_count++];
                memset(&t, 0, sizeof(t));
                t.number = number;
                t.control = desc[1] & 0x0F;
                t.start = lba;
            }
        }

        if (g_cd.track_count == 0 || g_cd.leadout == 0)
        {
            logmsg("-- CD table of contents has no tracks or no lead-out");
            continue;
        }

        // Tracks end where the next one starts
        bool valid = true;
        for (int i = 0; i < g_cd.track_count; i++)
        {
            cd_track_t &t = g_cd.tracks[i];
            t.end = (i + 1 < g_cd.track_count) ? g_cd.tracks[i + 1].start : g_cd.leadout;
            if (t.end <= t.start || (t.start & 0x80000000))
            {
                valid = false;
            }
        }

        if (!valid)
        {
            logmsg("-- CD table of contents has invalid track positions: ", bytearray(buf, len));
            continue;
        }

        return true;
    }

    return false;
}

// Read session information and shorten the last track before the last
// session so that the gap between the sessions is not read.
static void cdReadSessions()
{
    uint8_t *buf = scsiDev.data;
    uint8_t command[10] = {0x43, 0, 0x01, 0, 0, 0, 0, 0, 12, 0};
    int status = scsiInitiatorRunCommand(g_cd.target_id, command, sizeof(command),
                                         buf, 12, NULL, 0);
    if (status != 0)
    {
        if (status == 2)
        {
            uint8_t sense_key;
            scsiRequestSense(g_cd.target_id, &sense_key);
        }
        dbgmsg("-- CD session information not available");
        return;
    }

    // Drives that ignore the format field return the normal TOC instead
    uint32_t len = ((uint32_t)buf[0] << 8) | buf[1];
    if (len != 10)
    {
        dbgmsg("-- CD session information not supported");
        return;
    }

    uint8_t first_session = buf[2];
    uint8_t last_session = buf[3];
    uint8_t first_track_last_session = buf[6];
    if (last_session <= first_session)
    {
        return;
    }

    logmsg("-- CD has ", (int)(last_session - first_session + 1), " sessions, last session starts at track ",
           (int)first_track_last_session);

    if (last_session - first_session > 1)
    {
        logmsg("-- Only the gap before the last session is known, other gaps are detected by read errors");
    }

    for (int i = 1; i < g_cd.track_count; i++)
    {
        if (g_cd.tracks[i].number == first_track_last_session)
        {
            uint32_t gap = (last_session == 2) ? CD_SESSION_GAP_FIRST : CD_SESSION_GAP_LATER;
            cd_track_t &prev = g_cd.tracks[i - 1];
            if (prev.end - prev.start > gap)
            {
                prev.end -= gap;
            }
            break;
        }
    }
}

/*************************************
 * Probing                           *
 *************************************/

static uint32_t cdProbeLBA(const cd_track_t &t)
{
    uint32_t lba = t.start + 16;
    if (lba >= t.end) lba = t.start;
    return lba;
}

static bool cdHasSync(const uint8_t *sector)
{
    static const uint8_t sync[12] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    return memcmp(sector, sync, sizeof(sync)) == 0;
}

static cd_read_method_t cdProbeAudio(const cd_track_t &t, const char *vendor)
{
    cd_read_method_t methods[] = {CD_READ_READCD, CD_READ_D8, CD_READ_D4_NEC, CD_READ_10_D82, CD_READ_10_2352};
    uint32_t lba = cdProbeLBA(t);

    for (cd_read_method_t method : methods)
    {
        if (method == CD_READ_D4_NEC && strncmp(vendor, "NEC", 3) != 0)
        {
            // Opcode 0xD4 means different things on other vendors' drives
            continue;
        }

        if (cdReadSectors(method, true, lba, 1, scsiDev.data) == 0)
        {
            return method;
        }
    }

    return CD_READ_NONE;
}

static cd_read_method_t cdProbeData(const cd_track_t &t)
{
    uint32_t lba = cdProbeLBA(t);

    if (cdReadSectors(CD_READ_READCD, false, lba, 1, scsiDev.data) == 0 && cdHasSync(scsiDev.data))
    {
        return CD_READ_READCD;
    }

    if (cdReadSectors(CD_READ_10_2048, false, lba, 1, scsiDev.data) == 0)
    {
        return CD_READ_10_2048;
    }

    // Drive may not support MODE SELECT, try with its current block size
    uint32_t sectorcount, sectorsize;
    if (scsiInitiatorReadCapacity(g_cd.target_id, &sectorcount, &sectorsize) && sectorsize == CD_DATA_SECTOR)
    {
        uint8_t command[10] = {0x28, 0, 0, 0, 0, 0, 0, 0, 1, 0};
        writeBE32(&command[2], lba);
        if (scsiInitiatorRunCommand(g_cd.target_id, command, sizeof(command),
                                    scsiDev.data, CD_DATA_SECTOR, NULL, 0) == 0)
        {
            g_cd.cur_density = 0;
            g_cd.cur_blocklen = CD_DATA_SECTOR;
            return CD_READ_10_2048;
        }
    }

    return CD_READ_NONE;
}

initiator_cd_image_t scsiInitiatorCDProbe(int target_id, const char *vendor)
{
    memset(&g_cd, 0, sizeof(g_cd));
    g_cd.target_id = target_id;
    g_cd.cur_density = -1;
    g_cd.cur_blocklen = -1;

    long bincue = ini_getl("SCSI", "InitiatorCDBinCue", -1, CONFIGFILE);
    if (bincue == 0)
    {
        return INITIATOR_CD_IMAGE_ISO;
    }

    if (!cdReadTOC())
    {
        logmsg("-- Could not read the CD table of contents, imaging as .iso");
        return INITIATOR_CD_IMAGE_ISO;
    }

    cdReadSessions();

    int audio_tracks = 0;
    int first_audio = -1, first_data = -1;
    logmsg("-- CD has ", g_cd.track_count, " tracks, lead-out at LBA ", (int)g_cd.leadout);
    for (int i = 0; i < g_cd.track_count; i++)
    {
        const cd_track_t &t = g_cd.tracks[i];
        bool audio = cdIsAudio(t);
        if (audio)
        {
            audio_tracks++;
            if (first_audio < 0) first_audio = i;
        }
        else if (first_data < 0)
        {
            first_data = i;
        }
        dbgmsg("-- Track ", (int)t.number, audio ? " audio" : " data", " LBA ", (int)t.start,
               " - ", (int)t.end, " control ", t.control);
    }

    if (bincue < 0 && audio_tracks == 0 && g_cd.track_count == 1)
    {
        // Plain data CD
        return INITIATOR_CD_IMAGE_ISO;
    }

    if (first_audio >= 0)
    {
        g_cd.audio_method = cdProbeAudio(g_cd.tracks[first_audio], vendor);
        if (g_cd.audio_method == CD_READ_NONE)
        {
            logmsg("-- This drive does not support reading digital audio over SCSI, audio tracks will be left out");
        }
        else
        {
            logmsg("-- Reading audio tracks with ", cdMethodName(g_cd.audio_method));
        }
    }

    if (first_data >= 0)
    {
        g_cd.data_method = cdProbeData(g_cd.tracks[first_data]);
        if (g_cd.data_method == CD_READ_NONE)
        {
            logmsg("-- Could not read data tracks, they will be left out");
        }
        else
        {
            logmsg("-- Reading data tracks with ", cdMethodName(g_cd.data_method),
                   g_cd.data_method == CD_READ_READCD ? " (raw 2352 byte sectors)" : "");
        }
    }

    for (int i = 0; i < g_cd.track_count; i++)
    {
        const cd_track_t &t = g_cd.tracks[i];
        cd_read_method_t method = cdTrackMethod(t);
        if (method != CD_READ_NONE)
        {
            g_cd.total_sectors += t.end - t.start;
            g_cd.total_bytes += (uint64_t)(t.end - t.start) * cdSectorSize(method);
        }
    }

    if (g_cd.total_sectors == 0)
    {
        logmsg("-- None of the tracks on this CD can be read with this drive");
        return INITIATOR_CD_IMAGE_SKIP;
    }

    return INITIATOR_CD_IMAGE_BINCUE;
}

uint32_t scsiInitiatorCDTotalSectors()
{
    return g_cd.total_sectors;
}

uint64_t scsiInitiatorCDTotalBytes()
{
    return g_cd.total_bytes;
}

/*************************************
 * Imaging                           *
 *************************************/

bool scsiInitiatorCDBegin(FsFile *file, const char *bin_filename, uint8_t max_retry)
{
    g_cd.file = file;
    strncpy(g_cd.bin_filename, bin_filename, sizeof(g_cd.bin_filename) - 1);
    g_cd.max_retry = max_retry;
    g_cd.cur_track = 0;
    g_cd.track_started = false;
    g_cd.stored_frames = 0;
    g_cd.sectors_done = 0;
    g_cd.bad_sectors = 0;
    g_cd.skipped_sectors = 0;
    g_cd.start_time = millis();
    return true;
}

static void cdStartTrack(cd_track_t &t)
{
    t.file_frame = g_cd.stored_frames;
    t.read_end = t.end;
    t.data_mode = 1;
    g_cd.lba = t.start;
    g_cd.retrycount = 0;
    g_cd.failposition = 0;
    g_cd.bad_run_len = 0;
    g_cd.track_started = true;

    logmsg("-- Track ", (int)t.number, cdIsAudio(t) ? " (audio)" : " (data)",
           ": LBA ", (int)t.start, " - ", (int)(t.end - 1), ", ", (int)(t.end - t.start), " sectors");
}

static void cdFinishTrack()
{
    g_cd.cur_track++;
    g_cd.track_started = false;
}

// After a run of unreadable sectors, check whether the rest of the track
// is a gap that cannot be read. If so, remove the run from the file and
// stop reading the track. Returns true if the rest of the track was skipped.
static bool cdCheckUnreadableTail(cd_track_t &t, cd_read_method_t method)
{
    if (g_cd.cur_track >= g_cd.track_count - 1 || t.end - g_cd.bad_run_start > CD_MAX_GAP)
    {
        return false;
    }

    if (cdReadSectors(method, cdIsAudio(t), t.end - 1, 1, scsiDev.data) == 0)
    {
        // End of the track is readable, this is a damaged area
        return false;
    }

    if (!g_cd.file->truncate(g_cd.bad_run_filepos) || !g_cd.file->seek(g_cd.bad_run_filepos))
    {
        logmsg("-- CD image: failed to truncate .bin file");
        return false;
    }

    uint32_t skipped = t.end - g_cd.bad_run_start;
    logmsg("-- Track ", (int)t.number, ": last ", (int)skipped,
           " sectors cannot be read, leaving them out as a gap (pregap or session gap)");

    g_cd.stored_frames -= g_cd.bad_run_len;
    g_cd.bad_sectors -= g_cd.bad_run_len;
    g_cd.skipped_sectors += skipped;
    g_cd.sectors_done += t.end - g_cd.lba;
    t.read_end = g_cd.bad_run_start;
    g_cd.lba = t.end;
    g_cd.bad_run_len = 0;
    return true;
}

initiator_cd_step_t scsiInitiatorCDStep(uint32_t *sectors_done, uint32_t *sectors_in_batch)
{
    *sectors_in_batch = 0;
    *sectors_done = g_cd.sectors_done;

    if (g_cd.cur_track >= g_cd.track_count)
    {
        return INITIATOR_CD_DONE;
    }

    cd_track_t &t = g_cd.tracks[g_cd.cur_track];
    cd_read_method_t method = cdTrackMethod(t);

    if (!g_cd.track_started)
    {
        if (method == CD_READ_NONE)
        {
            logmsg("-- Track ", (int)t.number, cdIsAudio(t) ? " (audio)" : " (data)",
                   " cannot be read with this drive, leaving it out");
            t.file_frame = g_cd.stored_frames;
            t.read_end = t.start;
            cdFinishTrack();
            return INITIATOR_CD_CONTINUE;
        }

        cdStartTrack(t);
    }

    if (g_cd.lba >= t.end)
    {
        cdFinishTrack();
        return INITIATOR_CD_CONTINUE;
    }

    bool audio = cdIsAudio(t);
    uint32_t sectorsize = cdSectorSize(method);
    uint32_t count = sizeof(scsiDev.data) / sectorsize;
    if (count > t.end - g_cd.lba) count = t.end - g_cd.lba;
    if (g_cd.lba < g_cd.failposition || g_cd.bad_run_len > 0) count = 1;

    uint32_t time_start = millis();
    int status = cdReadSectors(method, audio, g_cd.lba, count, scsiDev.data);

    if (status == 0)
    {
        if (!audio && method == CD_READ_READCD && g_cd.lba == t.start && cdHasSync(scsiDev.data))
        {
            t.data_mode = scsiDev.data[15];
            dbgmsg("-- Track ", (int)t.number, " is data mode ", t.data_mode);
        }

        uint32_t len = count * sectorsize;
        if (g_cd.file->write(scsiDev.data, len) != len)
        {
            logmsg("-- CD image: SD card write failed");
            return INITIATOR_CD_FAILED;
        }

        g_cd.lba += count;
        g_cd.stored_frames += count;
        g_cd.sectors_done += count;
        g_cd.retrycount = 0;
        g_cd.bad_run_len = 0;

        uint32_t elapsed = millis() - time_start;
        if (elapsed == 0) elapsed = 1;
        logmsg("CD read succeeded, sectors done: ", (int)g_cd.sectors_done, " / ", (int)g_cd.total_sectors,
               " speed ", (int)(len / elapsed), " kB/s - ",
               (int)(100 * (uint64_t)g_cd.sectors_done / g_cd.total_sectors), "%");

        *sectors_in_batch = count;
        *sectors_done = g_cd.sectors_done;
        return INITIATOR_CD_CONTINUE;
    }

    if (status == -1)
    {
        logmsg("-- CD drive stopped responding");
        return INITIATOR_CD_FAILED;
    }

    // Retry, unless already in a run of unreadable sectors
    if (g_cd.bad_run_len == 0 && g_cd.retrycount < g_cd.max_retry)
    {
        g_cd.retrycount++;
        logmsg("Failed to read ", (int)count, " sectors at LBA ", (int)g_cd.lba,
               ", retrying ", g_cd.retrycount, "/", (int)g_cd.max_retry);
        if (g_cd.retrycount > 1 && count > 1)
        {
            logmsg("Multiple failures, retrying sector-by-sector");
            g_cd.failposition = g_cd.lba + count;
        }
        return INITIATOR_CD_CONTINUE;
    }

    // Give up on this sector, fill it with zeros
    if (g_cd.bad_run_len == 0)
    {
        g_cd.bad_run_start = g_cd.lba;
        g_cd.bad_run_filepos = g_cd.file->curPosition();
    }
    g_cd.bad_run_len++;
    g_cd.retrycount = 0;

    memset(scsiDev.data, 0, sectorsize);
    if (g_cd.file->write(scsiDev.data, sectorsize) != sectorsize)
    {
        logmsg("-- CD image: SD card write failed");
        return INITIATOR_CD_FAILED;
    }

    logmsg("Skipping unreadable sector at LBA ", (int)g_cd.lba);
    g_cd.lba++;
    g_cd.stored_frames++;
    g_cd.sectors_done++;
    g_cd.bad_sectors++;

    if (g_cd.bad_run_len == CD_BAD_RUN_GAP_CHECK)
    {
        cdCheckUnreadableTail(t, method);
    }

    *sectors_done = g_cd.sectors_done;
    return INITIATOR_CD_CONTINUE;
}

/*************************************
 * .cue sheet                        *
 *************************************/

static void cdFormatMSF(char *buf, size_t size, uint32_t frames)
{
    // Minutes are limited to 99 so that the result always fits in 9 bytes
    uint32_t minutes = frames / (75 * 60);
    if (minutes > 99) minutes = 99;
    snprintf(buf, size, "%02u:%02u:%02u",
             (unsigned)minutes,
             (unsigned)((frames / 75) % 60),
             (unsigned)(frames % 75));
}

static bool cdWriteCue()
{
    char cue_filename[sizeof(g_cd.bin_filename)];
    strncpy(cue_filename, g_cd.bin_filename, sizeof(cue_filename));
    char *ext = strrchr(cue_filename, '.');
    if (!ext || strlen(ext) != 4)
    {
        logmsg("-- CD image: unexpected file name ", g_cd.bin_filename);
        return false;
    }
    strcpy(ext, ".cue");

    FsFile cue = SD.open(cue_filename, O_WRONLY | O_CREAT | O_TRUNC);
    if (!cue.isOpen())
    {
        logmsg("-- Failed to open ", cue_filename, " for writing");
        return false;
    }

    char line[96];
    snprintf(line, sizeof(line), "FILE \"%s\" BINARY\r\n", g_cd.bin_filename);
    bool ok = cue.write(line, strlen(line)) == strlen(line);

    uint32_t unstored = 0;
    int written = 0;
    for (int i = 0; i < g_cd.track_count && ok; i++)
    {
        const cd_track_t &t = g_cd.tracks[i];
        if (t.read_end <= t.start)
        {
            // Track was not read, it becomes part of the next track's pregap
            continue;
        }

        cd_read_method_t method = cdTrackMethod(t);
        const char *mode;
        if (cdIsAudio(t))
            mode = "AUDIO";
        else if (method == CD_READ_10_2048)
            mode = "MODE1/2048";
        else if (t.data_mode == 2)
            mode = "MODE2/2352";
        else
            mode = "MODE1/2352";

        int len = snprintf(line, sizeof(line), "  TRACK %02d %s\r\n", t.number, mode);

        if (cdIsAudio(t) && (t.control & 0x0B))
        {
            len += snprintf(line + len, sizeof(line) - len, "    FLAGS%s%s%s\r\n",
                            (t.control & 0x02) ? " DCP" : "",
                            (t.control & 0x08) ? " 4CH" : "",
                            (t.control & 0x01) ? " PRE" : "");
        }

        // Sectors before this track that are not stored in the .bin file
        uint32_t pregap = t.start - (t.file_frame + unstored);
        if (pregap > 0)
        {
            char msf[16];
            cdFormatMSF(msf, sizeof(msf), pregap);
            len += snprintf(line + len, sizeof(line) - len, "    PREGAP %s\r\n", msf);
            unstored += pregap;
        }

        char msf[16];
        cdFormatMSF(msf, sizeof(msf), t.file_frame);
        len += snprintf(line + len, sizeof(line) - len, "    INDEX 01 %s\r\n", msf);

        ok = cue.write(line, len) == (size_t)len;
        written++;
    }

    cue.close();

    if (!ok)
    {
        logmsg("-- Failed to write ", cue_filename);
        return false;
    }

    logmsg("-- Wrote ", cue_filename, " with ", written, " tracks");
    return true;
}

void scsiInitiatorCDEnd()
{
    // Track that was interrupted by an error
    if (g_cd.track_started && g_cd.cur_track < g_cd.track_count)
    {
        g_cd.tracks[g_cd.cur_track].read_end = g_cd.lba;
        for (int i = g_cd.cur_track + 1; i < g_cd.track_count; i++)
        {
            g_cd.tracks[i].read_end = g_cd.tracks[i].start;
        }
    }
    else
    {
        for (int i = g_cd.cur_track; i < g_cd.track_count; i++)
        {
            g_cd.tracks[i].read_end = g_cd.tracks[i].start;
        }
    }

    g_cd.file->flush();
    cdWriteCue();

    uint32_t elapsed_s = (millis() - g_cd.start_time) / 1000;
    logmsg("CD image: ", (int)g_cd.stored_frames, " sectors in ", (int)elapsed_s, " seconds");
    if (g_cd.bad_sectors > 0)
    {
        logmsg("NOTE: ", (int)g_cd.bad_sectors, " sectors could not be read and were filled with zeros");
    }
    if (g_cd.skipped_sectors > 0)
    {
        logmsg("NOTE: ", (int)g_cd.skipped_sectors, " unreadable sectors at track ends were left out as gaps");
    }

    if (g_cd.mode_changed)
    {
        // Restore the normal block size for data reads
        g_cd.cur_density = -1;
        cdSetBlockMode(0, CD_DATA_SECTOR);
    }
}

#endif
