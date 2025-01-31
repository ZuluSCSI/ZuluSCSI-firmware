/*
 * Copyright (c) 2023 joshua stein <jcs@jcs.org>
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
#include <Arduino.h>
#include "ZuluSCSI_platform_network.h"
#include "ZuluSCSI_log.h"
#include "ZuluSCSI_config.h"
#include <scsi.h>
#include <network.h>

extern "C" {

#include <cyw43.h>
#include <pico/cyw43_arch.h>
#include <pico/unique_id.h>

#ifndef CYW43_IOCTL_GET_RSSI
#define CYW43_IOCTL_GET_RSSI (0xfe)
#endif

#define PICO_W_GPIO_LED_PIN 0
#define PICO_W_LED_ON() cyw43_arch_gpio_put(PICO_W_GPIO_LED_PIN, 1)
#define PICO_W_LED_OFF() cyw43_arch_gpio_put(PICO_W_GPIO_LED_PIN, 0)
#define PICO_W_LONG_BLINK_DELAY 200
#define PICO_W_SHORT_BLINK_DELAY 75

// A default DaynaPort-compatible MAC
static const char defaultMAC[] = { 0x00, 0x80, 0x19, 0xc0, 0xff, 0xee };

static bool network_in_use = false;

bool platform_network_supported()
{
	/* from cores/rp2040/RP2040Support.h */
#if !defined(PICO_CYW43_SUPPORTED)
	return false;
#else
	extern bool __isPicoW;
	return __isPicoW;
#endif
}

int platform_network_init(char *mac)
{
	pico_unique_board_id_t board_id;
	uint8_t set_mac[6], read_mac[6];

	if (!platform_network_supported())
		return -1;

	// long signal blink at network initialization
	PICO_W_LED_OFF();
	PICO_W_LED_ON();
	delay(PICO_W_LONG_BLINK_DELAY);
	PICO_W_LED_OFF();


	logmsg(" ");
	logmsg("=== Network Initialization ===");

	memset(wifi_network_list, 0, sizeof(wifi_network_list));

	cyw43_deinit(&cyw43_state);
	cyw43_init(&cyw43_state);

	if (mac == NULL || (mac[0] == 0 && mac[1] == 0 && mac[2] == 0 && mac[3] == 0 && mac[4] == 0 && mac[5] == 0))
	{
		mac = (char *)&set_mac;
		char octal_strings[8][4] = {0};
		memcpy(mac, defaultMAC, sizeof(set_mac));

		// retain Dayna vendor but use a device id specific to this board
		pico_get_unique_board_id(&board_id);

		dbgmsg("Unique board id: ", board_id.id[0], " ", board_id.id[1], " ", board_id.id[2], " ", board_id.id[3], " ",
									board_id.id[4], " ", board_id.id[5], " ", board_id.id[6], " ", board_id.id[7]);

		if (board_id.id[3] != 0 && board_id.id[4] != 0 && board_id.id[5] != 0)
		{
			mac[3] = board_id.id[3];
			mac[4] = board_id.id[4];
			mac[5] = board_id.id[5];
		}

		memcpy(scsiDev.boardCfg.wifiMACAddress, mac, sizeof(scsiDev.boardCfg.wifiMACAddress));
	}

	// setting the MAC requires libpico to be compiled with CYW43_USE_OTP_MAC=0
	memcpy(cyw43_state.mac, mac, sizeof(cyw43_state.mac));
	cyw43_arch_enable_sta_mode();

	cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, read_mac);
	char mac_hex_string[4*6] = {0};
	sprintf(mac_hex_string, "%02X:%02X:%02X:%02X:%02X:%02X", read_mac[0], read_mac[1], read_mac[2], read_mac[3], read_mac[4], read_mac[5]);
	logmsg("Wi-Fi MAC: ", mac_hex_string);
	if (memcmp(mac, read_mac, sizeof(read_mac)) != 0)
		logmsg("WARNING: Wi-Fi MAC is not what was requested (", mac_hex_string,"), is libpico not compiled with CYW43_USE_OTP_MAC=0?");

	network_in_use = true;

	return 0;
}

void platform_network_add_multicast_address(uint8_t *mac)
{
	int ret;

	if ((ret = cyw43_wifi_update_multicast_filter(&cyw43_state, mac, true)) != 0)
		logmsg( __func__, ": cyw43_wifi_update_multicast_filter: ", ret);
}

bool platform_network_wifi_join(char *ssid, char *password)
{
	int ret;

	if (!platform_network_supported())
		return false;

	if (password == NULL || password[0] == 0)
	{
		logmsg("Connecting to Wi-Fi SSID \"", ssid, "\" with no authentication");
		ret = cyw43_arch_wifi_connect_async(ssid, NULL, CYW43_AUTH_OPEN);
	}
	else
	{
		logmsg("Connecting to Wi-Fi SSID \"", ssid, "\" with WPA/WPA2 PSK");
		ret = cyw43_arch_wifi_connect_async(ssid, password, CYW43_AUTH_WPA2_MIXED_PSK);
	}

	if (ret != 0)
	{
		logmsg("Wi-Fi connection failed: ", ret);
	}
	else
	{
		// Short single blink at start of connection sequence
		PICO_W_LED_OFF();
		delay(PICO_W_SHORT_BLINK_DELAY);
		PICO_W_LED_ON();
		delay(PICO_W_SHORT_BLINK_DELAY);
		PICO_W_LED_OFF();
	}
	
	return (ret == 0);
}

void platform_network_poll()
{
	if (!network_in_use)
		return;

	scsiNetworkPurge();
	cyw43_arch_poll();
}

int platform_network_send(uint8_t *buf, size_t len)
{
	int ret = cyw43_send_ethernet(&cyw43_state, 0, len, buf, 0);
	if (ret != 0)
		logmsg("cyw43_send_ethernet failed: ", ret);

	return ret;
}

static int platform_network_wifi_scan_result(void *env, const cyw43_ev_scan_result_t *result)
{
	struct wifi_network_entry *entry = NULL;

	if (!result || !result->ssid_len || !result->ssid[0])
		return 0;

	for (int i = 0; i < WIFI_NETWORK_LIST_ENTRY_COUNT; i++)
	{
		// take first available
		if (wifi_network_list[i].ssid[0] == '\0')
		{
			entry = &wifi_network_list[i];
			break;
		}
		// or if we've seen this network before, use this slot
		else if (strcmp((char *)result->ssid, wifi_network_list[i].ssid) == 0)
		{
			entry = &wifi_network_list[i];
			break;
		}
	}

	if (!entry)
	{
		// no available slots, insert according to our RSSI
		for (int i = 0; i < WIFI_NETWORK_LIST_ENTRY_COUNT; i++)
		{
			if (result->rssi > wifi_network_list[i].rssi)
			{
				// shift everything else down
				for (int j = WIFI_NETWORK_LIST_ENTRY_COUNT - 1; j > i; j--)
					wifi_network_list[j] = wifi_network_list[j - 1];

				entry = &wifi_network_list[i];
				memset(entry, 0, sizeof(struct wifi_network_entry));
				break;
			}
		}
	}

	if (entry == NULL)
		return 0;

	if (entry->rssi == 0 || result->rssi > entry->rssi)
	{
		entry->channel = result->channel;
		entry->rssi = result->rssi;
	}
	if (result->auth_mode & 7)
		entry->flags = WIFI_NETWORK_FLAG_AUTH;
	strncpy(entry->ssid, (const char *)result->ssid, sizeof(entry->ssid));
	entry->ssid[sizeof(entry->ssid) - 1] = '\0';
	memcpy(entry->bssid, result->bssid, sizeof(entry->bssid));

	return 0;
}

int platform_network_wifi_start_scan()
{
	if (cyw43_wifi_scan_active(&cyw43_state))
		return -1;

	cyw43_wifi_scan_options_t scan_options = { 0 };
	memset(wifi_network_list, 0, sizeof(wifi_network_list));
	return cyw43_wifi_scan(&cyw43_state, &scan_options, NULL, platform_network_wifi_scan_result);
}

int platform_network_wifi_scan_finished()
{
	return !cyw43_wifi_scan_active(&cyw43_state);
}

void platform_network_wifi_dump_scan_list()
{
	struct wifi_network_entry *entry = NULL;
	
	for (int i = 0; i < WIFI_NETWORK_LIST_ENTRY_COUNT; i++)
	{
		entry = &wifi_network_list[i];

		if (entry->ssid[0] == '\0')
			break;
			
		logmsg("wifi[",i,"] = ",entry->ssid,", channel ",(int)entry->channel,", rssi ",(int)entry->rssi,
				", bssid ",(uint8_t) entry->bssid[0],":",(uint8_t) entry->bssid[1],":",(uint8_t) entry->bssid[2],":",
				(uint8_t) entry->bssid[3],":",(uint8_t) entry->bssid[4],":",(uint8_t) entry->bssid[5],", flags ", entry->flags);
	}
}

int platform_network_wifi_rssi()
{
	int32_t rssi = 0;

    cyw43_ioctl(&cyw43_state, CYW43_IOCTL_GET_RSSI, sizeof(rssi), (uint8_t *)&rssi, CYW43_ITF_STA);
	return rssi;
}

char * platform_network_wifi_ssid()
{
	struct ssid_t {
		uint32_t ssid_len;
		uint8_t ssid[32 + 1];
	} ssid;
	static char cur_ssid[32 + 1];

	memset(cur_ssid, 0, sizeof(cur_ssid));

	int ret = cyw43_ioctl(&cyw43_state, CYW43_IOCTL_GET_SSID, sizeof(ssid), (uint8_t *)&ssid, CYW43_ITF_STA);
	if (ret)
	{
		logmsg("Failed getting Wi-Fi SSID: ", ret);
		return NULL;
	}

	ssid.ssid[sizeof(ssid.ssid) - 1] = '\0';
	if (ssid.ssid_len < sizeof(ssid.ssid))
		ssid.ssid[ssid.ssid_len] = '\0';
	
	strlcpy(cur_ssid, (char *)ssid.ssid, sizeof(cur_ssid));
	return cur_ssid;
}

char * platform_network_wifi_bssid()
{
	static char bssid[6];

	memset(bssid, 0, sizeof(bssid));

	/* TODO */

	return bssid;
}

int platform_network_wifi_channel()
{
	int32_t channel = 0;

    cyw43_ioctl(&cyw43_state, CYW43_IOCTL_GET_CHANNEL, sizeof(channel), (uint8_t *)&channel, CYW43_ITF_STA);
	return channel;
}

// these override weakly-defined functions in pico-sdk

void cyw43_cb_process_ethernet(void *cb_data, int itf, size_t len, const uint8_t *buf)
{
	scsiNetworkEnqueue(buf, len);
}

void cyw43_cb_tcpip_set_link_down(cyw43_t *self, int itf)
{
	logmsg("Disassociated from Wi-Fi SSID \"",  (char *)self->ap_ssid,"\"");
}

void cyw43_cb_tcpip_set_link_up(cyw43_t *self, int itf)
{
	char *ssid = platform_network_wifi_ssid();

	if (ssid)
	{
		logmsg("Successfully connected to Wi-Fi SSID \"",ssid,"\"");
		// blink LED 3 times when connected
		PICO_W_LED_OFF();
		for (uint8_t i = 0; i < 3; i++)
		{
			delay(PICO_W_SHORT_BLINK_DELAY);
			PICO_W_LED_ON();
			delay(PICO_W_SHORT_BLINK_DELAY);
			PICO_W_LED_OFF();
		}
	}
}

}
#endif // ZULUSCSI_NETWORK