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

// Host PCM audio streaming over SCSI for the ZuluSCSI Blaster.
// A processor-type device (enabled by an "SNx.img" marker, like Daynaport's
// "NEx.img") that lets the host stream 44.1kHz/16-bit/stereo PCM to the DAC.
// See SCSI-sound.md for the full protocol.

#pragma once

// Vendor-specific SCSI opcodes (group 6, 6-byte CDB - no length registration).
#define AUDIO_SCSI_INFO   0xC3   // DATA_IN:  capabilities + live status (poll this)
#define AUDIO_SCSI_CTRL   0xC4   // no data:  CDB[1] = sub-op below
#define AUDIO_SCSI_WRITE  0xC5   // DATA_OUT: CDB[2..4] = byte length, append PCM

// AUDIO_SCSI_CTRL sub-ops (CDB[1])
#define AUDIO_CTRL_START      0x00   // begin streaming (preempts CD-DA)
#define AUDIO_CTRL_STOP       0x01   // stop, free the engine
#define AUDIO_CTRL_PAUSE      0x02
#define AUDIO_CTRL_RESUME     0x03
#define AUDIO_CTRL_FLUSH      0x04   // drop queued audio
#define AUDIO_CTRL_SET_VOLUME 0x05   // CDB[2] = level 0..255

// AUDIO_SCSI_INFO playback states (response byte 1)
#define AUDIO_STATE_STOPPED 0
#define AUDIO_STATE_PLAYING 1
#define AUDIO_STATE_PAUSED  2

#define AUDIO_SCSI_PROTOCOL_VERSION 1
#define AUDIO_SCSI_INFO_LEN         32   // bytes returned by AUDIO_SCSI_INFO

#ifdef __cplusplus
extern "C" {
#endif

// SCSI command handler for S2S_CFG_AUDIO targets. Returns 1 if the command was
// handled, 0 otherwise (so dispatch falls through). Mirrors scsiNetworkCommand().
int scsiAudioCommand(void);

#ifdef __cplusplus
}
#endif
