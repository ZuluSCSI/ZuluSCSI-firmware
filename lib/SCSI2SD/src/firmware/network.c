/*
 * Copyright (c) 2023-2026 joshua stein <jcs@jcs.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifdef ZULUSCSI_NETWORK

#include <string.h>
#include "scsi.h"
#include "scsi2sd_time.h"
#include "scsiPhy.h"
#include "config.h"
#include "network.h"
#include "crc32_ethernet.h"

extern int platform_network_send(uint8_t *buf, size_t len);

bool scsiNetworkEnabled = false;
struct scsiNetworkPacketQueue scsiNetworkInboundQueue;

struct __attribute__((packed)) wifi_network_entry wifi_network_list[WIFI_NETWORK_LIST_ENTRY_COUNT] = { 0 };

void scsiNetworkWifiScan(void)
{
	// initiate wi-fi scan
	scsiDev.dataLen = 1;
	int ret = platform_network_wifi_start_scan();
	scsiDev.data[0] = (ret < 0 ? ret : 1);
	scsiDev.phase = DATA_IN;
}

void scsiNetworkWifiComplete(void)
{
	// check for wi-fi scan completion
	scsiDev.dataLen = 1;
	scsiDev.data[0] = (platform_network_wifi_scan_finished() ? 1 : 0);
	scsiDev.phase = DATA_IN;
}

void scsiNetworkWifiScanResults(uint32_t size)
{
	// return wi-fi scan results
	if (!platform_network_wifi_scan_finished())
	{
		scsiDev.target->sense.code = ILLEGAL_REQUEST;
		scsiDev.target->sense.asc = INVALID_FIELD_IN_CDB;
		scsiDev.status = CHECK_CONDITION;
		scsiDev.phase = STATUS;
		return;
	}

	if (unlikely(size < 2))
	{
		scsiDev.target->sense.code = ILLEGAL_REQUEST;
		scsiDev.target->sense.asc = INVALID_FIELD_IN_CDB;
		scsiDev.status = CHECK_CONDITION;
		scsiDev.phase = STATUS;
		return;
	}

	int nets = 0;
	for (int i = 0; i < WIFI_NETWORK_LIST_ENTRY_COUNT; i++)
	{
		if (wifi_network_list[i].ssid[0] == '\0')
			break;
		nets++;
	}

	if (nets) {
		unsigned int netsize = sizeof(struct wifi_network_entry) * nets;
		if (netsize + 2 > sizeof(scsiDev.data))
		{
			LOGMSG_F("WARNING: wifi_network_list is bigger than scsiDev.data, truncating", 0);
			netsize = sizeof(scsiDev.data) - 2;
			netsize -= (netsize % (sizeof(struct wifi_network_entry)));
		}
		if (netsize + 2 > size)
		{
			LOGMSG_F("WARNING: wifi_network_list is bigger than requested dataLen, truncating", 0);
			netsize = size - 2;
			netsize -= (netsize % (sizeof(struct wifi_network_entry)));
		}
		scsiDev.data[0] = (netsize >> 8) & 0xff;
		scsiDev.data[1] = netsize & 0xff;
		memcpy(scsiDev.data + 2, wifi_network_list, netsize);
		scsiDev.dataLen = netsize + 2;
	}
	else
	{
		scsiDev.data[0] = 0;
		scsiDev.data[1] = 0;
		scsiDev.dataLen = 2;
	}

	scsiDev.phase = DATA_IN;
}

void scsiNetworkWifiInfo(void)
{
	// return current wi-fi information
	struct wifi_network_entry wifi_cur = { 0 };
	int entrysize = sizeof(wifi_cur);

	char *ssid = platform_network_wifi_ssid();
	if (ssid != NULL)
		strlcpy(wifi_cur.ssid, ssid, sizeof(wifi_cur.ssid));

	char *bssid = platform_network_wifi_bssid();
	if (bssid != NULL)
		memcpy(wifi_cur.bssid, bssid, sizeof(wifi_cur.bssid));

	wifi_cur.rssi = platform_network_wifi_rssi();

	wifi_cur.channel = platform_network_wifi_channel();

	scsiDev.data[0] = (entrysize >> 8) & 0xff;
	scsiDev.data[1] = entrysize & 0xff;
	memcpy(scsiDev.data + 2, (char *)&wifi_cur, entrysize);
	scsiDev.dataLen = entrysize + 2;
	scsiDev.phase = DATA_IN;
}

void scsiNetworkWifiJoin(uint32_t size)
{
	// set current wi-fi network
	struct wifi_join_request req = { 0 };

	if (size != sizeof(req)) {
		LOGMSG_F("wifi_join_request bad size (%zu != %zu), ignoring", size, sizeof(req));
		scsiDev.status = CHECK_CONDITION;
		scsiDev.phase = STATUS;
		return;
	}

	int parityError = 0;
	scsiEnterPhase(DATA_OUT);
	scsiRead((uint8_t *)&req, sizeof(req), &parityError);
	DBGMSG_F("%s: read join request from host:", __func__);
	DBGMSG_BUF(scsiDev.data, size);
	platform_network_wifi_join(req.ssid, req.key, false);

	scsiDev.status = GOOD;
	scsiDev.phase = STATUS;
}

int scsiNetworkCommand()
{
	int parityError, done, idx, total;
	long len;
	uint32_t size = (scsiDev.cdb[3] << 8) + scsiDev.cdb[4];
	uint8_t command = scsiDev.cdb[0];

	DBGMSG_F("------ in scsiNetworkCommand with command 0x%02x (size %d)", command, size);

	switch (command) {
	case 0x08:
	{
		// read(6)
		// CDB[5] bit 6 indicates transfer mode from the host driver:
		//   0x80 (bit 6 clear) = polled mode (old SCSI Manager, VM present) -> single packet
		//   0xC0 (bit 6 set)   = blind mode (new SCSI Manager, no VM) -> multi-packet OK
		// When bit 6 is clear, we send only one packet per READ(6) to minimize SCSI bus
		// hold time, allowing the VM pager to access the bus between transactions.

		int multiPacket = (scsiDev.cdb[5] & 0x40) != 0;
		if (unlikely(size == 1))
		{
			scsiDev.status = CHECK_CONDITION;
			scsiDev.phase = STATUS;
			break;
		}

		// if we have nothing to send, just return early
		if (scsiNetworkInboundQueue.readIndex == scsiNetworkInboundQueue.writeIndex)
		{
			memset(scsiDev.data, 0, 6);
			scsiEnterPhase(DATA_IN);
			scsiWrite(scsiDev.data, 6);
			while (!scsiIsWriteFinished(NULL))
			{
				platform_poll();
			}
			scsiFinishWrite();
			scsiDev.status = GOOD;
			scsiDev.phase = STATUS;
			break;
		}

		scsiEnterPhase(DATA_IN);

		for (done = 0, total = 0; !done; )
		{
			idx = scsiNetworkInboundQueue.readIndex;
			len = scsiNetworkInboundQueue.sizes[idx];
			total += len + 6;  // packet data + 6-byte header;

			if (scsiNetworkInboundQueue.readIndex == NETWORK_PACKET_QUEUE_SIZE - 1)
			{
				scsiNetworkInboundQueue.readIndex = 0;
			}
			else
			{
				scsiNetworkInboundQueue.readIndex++;
			}

			done = (scsiNetworkInboundQueue.readIndex == scsiNetworkInboundQueue.writeIndex);

		// In polled mode (bit 6 clear), only send one packet per READ(6)
			// to avoid holding the SCSI bus while the VM pager needs it.
			if (!done && !multiPacket)
			{
				done = 1;
			}

			// Don't exceed the transfer size requested by the host
			if (!done && total + DAYNAPORT_SCSI_PACKET_MAX + 6 > size)
			{
				done = 1;
			}

			// Don't tie up the SCSI bus too long even in multi-packet mode
			if (!done && total >= (DAYNAPORT_SCSI_PACKET_MAX + 6) * 2)
			{
				done = 1;
			}

			scsiDev.data[0] = (len >> 8) & 0xff;
			scsiDev.data[1] = len & 0xff;
			scsiDev.data[2] = 0;
			scsiDev.data[3] = 0;
			scsiDev.data[4] = 0;
			scsiDev.data[5] = (done ? 0 : 0x10);

			scsiWrite(scsiDev.data, 6);
			while (!scsiIsWriteFinished(NULL))
			{
				platform_poll();
			}
			scsiFinishWrite();

			// DaynaPort Mac driver needs a short delay after reading size and flags.
			// Timing matches real DaynaPORT SCSI/Link-3 behavior observed on a SCSI bus analyzer.
			sleep_us(75);

			scsiWrite(scsiNetworkInboundQueue.packets[idx], len);
			while (!scsiIsWriteFinished(NULL))
			{
				platform_poll();
			}
			scsiFinishWrite();

			if (!done)
			{
				// DaynaPort Mac driver needs a delay between packets.
				// Timing matches real DaynaPORT SCSI/Link-3 behavior observed on a SCSI bus analyzer.
				sleep_us(300);
			}
		}

		scsiDev.status = GOOD;
		scsiDev.phase = STATUS;
		break;
	}

	case 0x09:
		// read mac address and stats
		memcpy(scsiDev.data, scsiDev.boardCfg.wifiMACAddress, sizeof(scsiDev.boardCfg.wifiMACAddress));
		memset(scsiDev.data + sizeof(scsiDev.boardCfg.wifiMACAddress), 0,
			sizeof(scsiDev.data) - sizeof(scsiDev.boardCfg.wifiMACAddress));

		// three 32-bit counters expected to follow, just return zero for all
		scsiDev.dataLen = 18;
		scsiDev.phase = DATA_IN;
		break;

	case 0x0a:
		// write(6)
		scsiEnterPhase(DATA_OUT);

		for (;;)
		{
			if (scsiDev.cdb[5] == 0x0)
			{
				// no preamble, single packet
				len = size;
				DBGMSG_F("no-preamble write, size %ld (cdb %02x %02x %02x %02x %02x %02x)", len,
					scsiDev.cdb[0], scsiDev.cdb[1], scsiDev.cdb[2], scsiDev.cdb[3], scsiDev.cdb[4], scsiDev.cdb[5]);
			}
			else
			{
				// read size of this packet
				scsiRead(scsiDev.data, 4, &parityError);
				if (parityError)
				{
					DBGMSG_F("%s: read of size from host had parity error %d", __func__, parityError);
				}

				len = (scsiDev.data[0] << 8) + scsiDev.data[1];
				if (len == 0)
				{
					// final packet was read in previous iteration
					break;
				}
			}

			if (len > NETWORK_PACKET_MAX_SIZE)
			{
				DBGMSG_F("%s: attempt to write(6) packet of size %zu", __func__, len);
				len = NETWORK_PACKET_MAX_SIZE;
			}

			scsiRead(scsiDev.data, len, &parityError);
			if (parityError)
			{
				DBGMSG_F("%s: read from host of size %zu had parity error %d", __func__, size, parityError);
			}

			platform_network_send(scsiDev.data, len);      

			if (scsiDev.cdb[5] == 0x0)
			{
				// single-packet mode
				break;
			}
		}

		scsiDev.status = GOOD;
		scsiDev.phase = STATUS;
		break;

	case 0x0c:
		// set interface mode (ignored)
		//broadcasts = (scsiDev.cdb[4] == 0x04);
		break;

	case 0x0d:
		// add multicast addr to network filter
		memset(scsiDev.data, 0, sizeof(scsiDev.data));
		scsiEnterPhase(DATA_OUT);
		parityError = 0;
		scsiRead(scsiDev.data, size, &parityError);

		DBGMSG_F("%s: adding multicast address %02x:%02x:%02x:%02x:%02x:%02x", __func__,
			  scsiDev.data[0], scsiDev.data[1], scsiDev.data[2], scsiDev.data[3], scsiDev.data[4], scsiDev.data[5]);


		platform_network_add_multicast_address(scsiDev.data);

		scsiDev.status = GOOD;
		scsiDev.phase = STATUS;
		break;

	case 0x0e:
		// toggle interface
		if (scsiDev.cdb[5] & 0x80)
		{
			DBGMSG_F("%s: enable interface", __func__);
			scsiNetworkEnabled = true;
			memset(&scsiNetworkInboundQueue, 0, sizeof(scsiNetworkInboundQueue));
		}
		else
		{
			DBGMSG_F("%s: disable interface", __func__);
			scsiNetworkEnabled = false;
		}
		break;

	case 0x1a:
		// mode sense (ignored)
		break;

	case 0x40:
		// set MAC (ignored)
		scsiDev.dataLen = 6;
		scsiDev.phase = DATA_OUT;
		break;

	case 0x80:
		// set mode (ignored)
		break;

	// custom wifi commands all using the same opcode, with a sub-command in cdb[2]
	case SCSI_NETWORK_WIFI_CMD:
		DBGMSG_F("------ in scsiNetworkCommand with wi-fi command 0x%02x (size %d)", scsiDev.cdb[2], size);

		switch (scsiDev.cdb[1]) {
		case SCSI_NETWORK_WIFI_CMD_SCAN:
			scsiNetworkWifiScan();
			break;
		case SCSI_NETWORK_WIFI_CMD_COMPLETE:
			scsiNetworkWifiComplete();
			break;
		case SCSI_NETWORK_WIFI_CMD_SCAN_RESULTS:
			scsiNetworkWifiScanResults(size);
			break;
		case SCSI_NETWORK_WIFI_CMD_INFO:
			scsiNetworkWifiInfo();
			break;
		case SCSI_NETWORK_WIFI_CMD_JOIN:
			scsiNetworkWifiJoin(size);
			break;
		}
		break;
	}

	return 1;
}

int scsiNetworkEnqueue(const uint8_t *buf, size_t len)
{
	if (!scsiNetworkEnabled)
		return 0;

	if (len + 4 > sizeof(scsiNetworkInboundQueue.packets[0]))
	{
		DBGMSG_F("%s: dropping incoming network packet, too large (%zu > %zu)", __func__, len, sizeof(scsiNetworkInboundQueue.packets[0]));
		return 0;
	}

	memcpy(scsiNetworkInboundQueue.packets[scsiNetworkInboundQueue.writeIndex], buf, len);

	if (len < 60) {
		// packets the host reads have to be at least 64 bytes, so pad before we CRC and add to queue
		memset(scsiNetworkInboundQueue.packets[scsiNetworkInboundQueue.writeIndex] + len, 0, 60 - len);
		len += (60 - len);
	}

	uint32_t crc = crc32(scsiNetworkInboundQueue.packets[scsiNetworkInboundQueue.writeIndex], len);
	scsiNetworkInboundQueue.packets[scsiNetworkInboundQueue.writeIndex][len] = crc & 0xff;
	scsiNetworkInboundQueue.packets[scsiNetworkInboundQueue.writeIndex][len + 1] = (crc >> 8) & 0xff;
	scsiNetworkInboundQueue.packets[scsiNetworkInboundQueue.writeIndex][len + 2] = (crc >> 16) & 0xff;
	scsiNetworkInboundQueue.packets[scsiNetworkInboundQueue.writeIndex][len + 3] = (crc >> 24) & 0xff;

	scsiNetworkInboundQueue.sizes[scsiNetworkInboundQueue.writeIndex] = len + 4;

	if (scsiNetworkInboundQueue.writeIndex == NETWORK_PACKET_QUEUE_SIZE - 1)
		scsiNetworkInboundQueue.writeIndex = 0;
	else
		scsiNetworkInboundQueue.writeIndex++;

	if (scsiNetworkInboundQueue.writeIndex == scsiNetworkInboundQueue.readIndex)
	{
		DBGMSG_F("%s: dropping packets in ring, write index caught up to read index", __func__);
	}

	return 1;
}

#endif // ZULUSCSI_NETWORK
