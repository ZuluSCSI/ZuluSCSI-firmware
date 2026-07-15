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
#ifdef ZULUCONTROL_FIRMWARE

#include "ZuluSCSI_WebUI_i2c_master_dma.h"
#include "ZuluSCSI_WebUI_I2CServer.h"
#include <hardware/dma.h>
#include <hardware/structs/i2c.h>
#include <hardware/regs/i2c.h>
#include <crc32_ethernet.h>

namespace {

i2c_inst_t* g_i2c = nullptr;
int g_hdr_channel = -1;
int g_data_channel = -1;
bool g_initialized = false;

// Referenced asynchronously by DMA hardware -- must have a stable address for the
// duration of a transfer, so these are static/file-scope rather than stack-local.
// Every entry is widened to 16 bits so the STOP control bit (bit 9 of
// IC_DATA_CMD) can be embedded in the last payload word (mirrors
// arduino-pico's own TwoWire::writeReadAsync and ZuluIDE-firmware's
// i2c_master_dma.cpp -- a narrower 8-bit write was tried there and found to
// intermittently corrupt the transaction, see that file's history).
uint16_t g_header_buf[3];
uint16_t g_data_buf[WEBUI_FW_UPGRADE_MAX_CHUNK];

uint32_t Crc32(const uint8_t* data, size_t length) {
    return crc32(data, length);
}

// Polls until `channel` finishes, an I2C TX_ABRT is flagged, or `until` is reached.
// On timeout or abort, aborts the DMA channel and returns false.
bool WaitForChannelOrAbort(i2c_inst_t* i2c, uint channel, absolute_time_t until) {
    while (dma_channel_is_busy(channel)) {
        if (i2c_get_hw(i2c)->raw_intr_stat & I2C_IC_RAW_INTR_STAT_TX_ABRT_BITS) {
            i2c_get_hw(i2c)->clr_tx_abrt;
            dma_channel_abort(channel);
            return false;
        }
        if (absolute_time_diff_us(get_absolute_time(), until) < 0) {
            dma_channel_abort(channel);
            return false;
        }
    }

    // dma_channel_is_busy() only reflects the DMA engine finishing *writing*
    // into the TX FIFO -- not the I2C peripheral finishing *shifting those
    // bytes out over the wire*. The 16-entry TX FIFO lets DMA fill it far
    // faster than the wire rate can drain it, so at this point there can
    // still be a tail of real transmission in flight. Wait for the actual
    // STOP condition (proof the whole transaction, including that tail,
    // genuinely completed on the wire) before declaring success.
    while (!(i2c_get_hw(i2c)->raw_intr_stat &
             (I2C_IC_RAW_INTR_STAT_STOP_DET_BITS | I2C_IC_RAW_INTR_STAT_TX_ABRT_BITS))) {
        if (absolute_time_diff_us(get_absolute_time(), until) < 0) {
            return false;
        }
    }
    if (i2c_get_hw(i2c)->raw_intr_stat & I2C_IC_RAW_INTR_STAT_TX_ABRT_BITS) {
        i2c_get_hw(i2c)->clr_tx_abrt;
        return false;
    }
    i2c_get_hw(i2c)->clr_stop_det;
    return true;
}

}  // namespace

// dma_claim_unused_channel() is safe here: unlike ZuluIDE-firmware (where
// some DMA channels are reserved by fixed-number convention only, never
// registered with the SDK's claim bitmap, so dma_claim_unused_channel()
// could silently hand out an in-use channel), every DMA user in this
// codebase -- scsi_accel_target(_wide).cpp, sdio.cpp, audio_i2s.cpp -- calls
// dma_channel_claim()/dma_channel_unclaim() around its actual channel
// numbers. Called once at the start of a firmware-upgrade attempt (boot
// time, after the SD card and SCSI targets are already initialized), so any
// channels already in real use are correctly reflected in the claim bitmap.
bool webuiI2CDmaInit(i2c_inst_t* i2c) {
    g_i2c = i2c;
    g_hdr_channel = dma_claim_unused_channel(false);
    if (g_hdr_channel < 0) {
        return false;
    }
    g_data_channel = dma_claim_unused_channel(false);
    if (g_data_channel < 0) {
        dma_channel_unclaim(g_hdr_channel);
        g_hdr_channel = -1;
        return false;
    }
    g_initialized = true;
    return true;
}

void webuiI2CDmaDeinit() {
    if (g_hdr_channel >= 0) {
        dma_channel_unclaim(g_hdr_channel);
        g_hdr_channel = -1;
    }
    if (g_data_channel >= 0) {
        dma_channel_unclaim(g_data_channel);
        g_data_channel = -1;
    }
    g_initialized = false;
}

bool webuiI2CDmaSendChunk(uint8_t cmd, const uint8_t* payload, uint16_t length,
                          uint32_t* out_sent_crc, absolute_time_t until) {
    if (!g_initialized || length == 0 || length > WEBUI_FW_UPGRADE_MAX_CHUNK) {
        return false;
    }

    i2c_inst_t* i2c = g_i2c;

    g_header_buf[0] = cmd;
    g_header_buf[1] = (length >> 8) & 0xFF;
    g_header_buf[2] = length & 0xFF;
    for (uint16_t i = 0; i < length; i++) {
        g_data_buf[i] = payload[i];
    }
    // STOP goes on the last payload word -- there is no trailing CRC on the
    // wire. The client independently computes its own CRC32 of what it
    // received and reports it in the ACK; the caller compares that against
    // *out_sent_crc to decide whether to retry.
    g_data_buf[length - 1] |= I2C_IC_DATA_CMD_STOP_BITS;

    dma_channel_config hdr_cfg = dma_channel_get_default_config(g_hdr_channel);
    channel_config_set_transfer_data_size(&hdr_cfg, DMA_SIZE_16);
    channel_config_set_read_increment(&hdr_cfg, true);
    channel_config_set_write_increment(&hdr_cfg, false);
    channel_config_set_dreq(&hdr_cfg, i2c_get_dreq(i2c, true));
    channel_config_set_chain_to(&hdr_cfg, g_data_channel);
    dma_channel_configure(g_hdr_channel, &hdr_cfg, &i2c_get_hw(i2c)->data_cmd, g_header_buf, 3, false);

    dma_channel_config data_cfg = dma_channel_get_default_config(g_data_channel);
    channel_config_set_transfer_data_size(&data_cfg, DMA_SIZE_16);
    channel_config_set_read_increment(&data_cfg, true);
    channel_config_set_write_increment(&data_cfg, false);
    channel_config_set_dreq(&data_cfg, i2c_get_dreq(i2c, true));
    channel_config_set_chain_to(&data_cfg, g_data_channel);  // no further chaining
    dma_channel_configure(g_data_channel, &data_cfg, &i2c_get_hw(i2c)->data_cmd, g_data_buf, length, false);

    i2c_get_hw(i2c)->enable = 0;
    i2c_get_hw(i2c)->tar = WEBUI_I2C_SLAVE_ADDR;
    i2c_get_hw(i2c)->dma_cr = I2C_IC_DMA_CR_TDMAE_BITS;
    i2c_get_hw(i2c)->enable = 1;

    // Clear any stale STOP_DET/TX_ABRT left set by a prior transaction (e.g. a
    // Wire-based control message) -- WaitForChannelOrAbort below treats these
    // as proof *this* transaction completed, so a leftover bit would make it
    // return success (or a false abort) instantly, before any real bytes go out.
    i2c_get_hw(i2c)->clr_stop_det;
    i2c_get_hw(i2c)->clr_tx_abrt;

    dma_channel_start(g_hdr_channel);  // chains automatically into g_data_channel

    // The DMA engine now streams the header+payload out over the wire
    // autonomously, paced by the I2C TX DREQ, with no further CPU
    // involvement until it's done. Compute the payload's software CRC32 on
    // the CPU right now, while that transfer is in flight, instead of
    // blocking on it beforehand -- the wire transfer dwarfs the CRC loop, so
    // this hides the entire CRC cost under the DMA transfer instead of
    // serializing it onto the critical path. Reads `payload` (untouched by
    // DMA, which reads from the separate widened g_data_buf), so there's no
    // data race with the in-flight transfer.
    uint32_t crc = Crc32(payload, length);

    bool ok = WaitForChannelOrAbort(i2c, g_data_channel, until);
    i2c_get_hw(i2c)->dma_cr = 0;

    if (!ok) {
        return false;
    }

    if (out_sent_crc) *out_sent_crc = crc;
    return true;
}

#endif
