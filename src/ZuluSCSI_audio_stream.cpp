/**
 * ZuluSCSI™ - Copyright (c) 2025 Rabbit Hole Computing™
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

#ifdef ENABLE_AUDIO_STREAM

#include "ZuluSCSI_audio_stream.h"
#include "ZuluSCSI_audio.h"
#include "ZuluSCSI_log.h"
#include <string.h>

extern "C" {
#include <scsi.h>
#include <sense.h>
}

static void put_le32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

// Called after the AUDIO_WRITE DATA_OUT phase: scsiDev.data holds the received
// PCM, scsiDev.dataLen its length. Push into the stream buffer. Overflow (no free
// slot) is dropped and surfaced via AUDIO_SCSI_INFO, not reported as an error.
static void doAudioWrite(void) {
    audio_stream_write(scsiDev.data, (uint32_t)scsiDev.dataLen);
    scsiDev.phase = STATUS;
}

static void audioInfoResponse(uint8_t owner) {
    uint8_t* d = scsiDev.data;
    memset(d, 0, AUDIO_SCSI_INFO_LEN);

    uint8_t state = AUDIO_STATE_STOPPED;
    if (audio_stream_is_active())
        state = (audio_get_status_code(owner) == ASC_PAUSED) ? AUDIO_STATE_PAUSED
                                                             : AUDIO_STATE_PLAYING;

    uint16_t vol = audio_get_volume(owner);

    d[0] = AUDIO_SCSI_PROTOCOL_VERSION;
    d[1] = state;
    d[2] = 2;    // channels (stereo)
    d[3] = 16;   // bits per sample
    put_le32(d + 4,  44100);
    put_le32(d + 8,  audio_stream_total_size());
    put_le32(d + 12, audio_stream_bytes_free());
    put_le32(d + 16, audio_stream_bytes_queued());
    put_le32(d + 20, audio_stream_underruns());
    put_le32(d + 24, audio_stream_overflows());
    d[28] = (uint8_t)(((vol & 0xFF) + (vol >> 8)) / 2);  // averaged 0..255

    int alloc = scsiDev.cdb[4];
    if (alloc == 0 || alloc > AUDIO_SCSI_INFO_LEN) alloc = AUDIO_SCSI_INFO_LEN;
    scsiDev.dataLen = alloc;
    scsiDev.phase = DATA_IN;
}

static void audioControl(uint8_t owner) {
    switch (scsiDev.cdb[1]) {
        case AUDIO_CTRL_START:  audio_stream_start(owner);     break;
        case AUDIO_CTRL_STOP:   audio_stop(owner);             break;
        case AUDIO_CTRL_PAUSE:  audio_set_paused(owner, true); break;
        case AUDIO_CTRL_RESUME: audio_set_paused(owner, false); break;
        case AUDIO_CTRL_FLUSH:  audio_stream_flush();          break;
        case AUDIO_CTRL_SET_VOLUME: {
            uint8_t level = scsiDev.cdb[2];
            audio_set_volume(owner, (uint16_t)((level << 8) | level));
            break;
        }
        default:
            scsiDev.target->sense.code = ILLEGAL_REQUEST;
            scsiDev.target->sense.asc = INVALID_FIELD_IN_CDB;
            scsiDev.status = CHECK_CONDITION;
            scsiDev.phase = STATUS;
            return;
    }
    scsiDev.status = GOOD;
    scsiDev.phase = STATUS;
}

extern "C" int scsiAudioCommand(void) {
    uint8_t owner = scsiDev.target->targetId;

    switch (scsiDev.cdb[0]) {
        case AUDIO_SCSI_INFO:
            audioInfoResponse(owner);
            return 1;

        case AUDIO_SCSI_CTRL:
            audioControl(owner);
            return 1;

        case AUDIO_SCSI_WRITE: {
            uint32_t len = ((uint32_t)scsiDev.cdb[2] << 16)
                         | ((uint32_t)scsiDev.cdb[3] << 8)
                         |  (uint32_t)scsiDev.cdb[4];
            uint32_t slot = audio_stream_slot_size();
            if (len > slot) len = slot;   // one slot per write (v1)
            if (len == 0) {
                scsiDev.status = GOOD;
                scsiDev.phase = STATUS;
                return 1;
            }
            scsiDev.dataLen = len;
            scsiDev.phase = DATA_OUT;
            scsiDev.postDataOutHook = doAudioWrite;
            return 1;
        }

        default:
            return 0;  // not an audio command; let dispatch fall through
    }
}

#endif // ENABLE_AUDIO_STREAM
