/* TODO - Header. By ZZJ. */

#include <SdFat.h>
#include <device/usbd.h>
#include <hardware/gpio.h>
#include "ZuluSCSI_platform.h"
#include "ZuluSCSI_log.h"
#include "rp_msc_reader.h"
#include "msc.h"
#include "msc_device.h"

#ifdef PLATFORM_CARDREADER

#if CFG_TUD_MSC_EP_BUFSIZE < SD_SECTOR_SIZE
  #error "CFG_TUD_MSC_EP_BUFSIZE is too small! It needs to be at least 512 (SD_SECTOR_SIZE)"
#endif

/*
TODO/Issues:

* handle card removal...
** if detect, just exit CR mode?

* images not present: allow to enter card reader mode
** -> question for zulu team
** and other alternative entry modes

* How to fix tinyusb MSC buffer in a sustainable manner 
** -> question for zulu team
** if pull the source into the tree, we can perhaps repurpose static buffers for other functions
** WHY: TinyUSB including in RP2040 SDK is compiled with CFG_TUD_MSC_EP_BUFSIZE of 64. This does
** not allow for reading an entire sector which leads to awful performance. 
** Larger is better, 4096 is a good all around with about 600kb/s read and 500kb/s write 
** max is about 650kb/s R/W with a huge buffer, due to USB stack and/or hardware limitations.

* support other platforms
** do they support and/or integrate tinyusb? if so, this may be trivial

* network buffer has been reduced to 10 temporarily to free up SRAM
** original device has 32K SRAM in it
** on an accelerated SE/30, no major difference to AFP throughput (10 -> 60KB/s, 18 -> 63KB/s)
** -> question for zulu team
*/

// private constants/enums
#define SD_SECTOR_SIZE 512

enum LEDState { LED_SOLIDON = 0, LED_BLINK_FAST, LED_BLINK_SLOW };

// external global SD variable
extern SdFs SD;

// private globals
volatile byte LEDMode;
volatile bool unitReady = false;

// returns true if card reader mode should be entered
// this probably become part of zuluscsi proper, if this code is upstreamed
bool shouldEnterReader() {

#ifdef ZULUSCSI_PICO
  // check if we're USB powered, if not, exit immediately
  // pin on the wireless module, see https://github.com/earlephilhower/arduino-pico/discussions/835
  if (!digitalRead(34)) 
    return false;
#endif

  logmsg("Waiting for USB enumeration to enter Card Reader mode.");

  // wait for up to a second to be enumerated
  uint32_t start = millis();
  while (!tud_connected() && ((uint32_t)(millis() - start) < CR_ENUM_TIMEOUT)) 
    delay(100);

  // tud_connected returns True if just got out of Bus Reset and received the very first data from host
  // https://github.com/hathach/tinyusb/blob/master/src/device/usbd.h#L63
  return tud_connected();
}

// card reader operation loop
// assumption that SD card was enumerated and is working
void runCardReader() {

  // blink twice to indicate entering card reader mode.
  LED_ON();
  delay(50);
  LED_OFF();
  delay(50);
  LED_ON();
  delay(50);
  LED_OFF();

  logmsg("SD card reader mode entered.");
  logmsg("USB MSC buffer size: ", CFG_TUD_MSC_EP_BUFSIZE);

  // MSC is ready for read/write
  // we don't need any prep, but the var is requried as the MSC callbacks are always active
  unitReady = true;

  // steady state operation / indication loop
  // led remains steady on
  while(unitReady) {
    platform_reset_watchdog(); // also sends log to USB serial

    // blink LED according to access type
    switch (LEDMode) {
      case LED_BLINK_FAST:
        LED_OFF();
        delay(30);
        break;
      case LED_BLINK_SLOW:
        delay(30);
        LED_OFF();
        delay(100);
        break;
    }

    // LED always on in card reader mode
    LEDMode = LED_SOLIDON;
	  LED_ON(); 
    delay(30);
  }

  logmsg("Card ejected, leaving cardreader mode.");

  // blink twice to indicate we are leaving reader mode
  LED_OFF();
  delay(200);
  LED_ON();
  delay(200);
  LED_OFF();
  delay(200);
  LED_ON();
  delay(200);

  // exit with LED off (default state)
  LED_OFF();
}

/* TinyUSB mass storage callbacks follow */

// usb framework checks this func exists for mass storage config. no code needed.
void __USBInstallMassStorage() { }

// Invoked when received SCSI_CMD_INQUIRY
// fill vendor id, product id and revision with string up to 8, 16, 4 characters respectively
extern "C" void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8],
                        uint8_t product_id[16], uint8_t product_rev[4]) {

  // TODO: We could/should use strings from the platform, but they are too long
  const char vid[] = "ZuluSCSI";
  const char pid[] = "Pico"; 
  const char rev[] = "1.0";

  memcpy(vendor_id, vid, tu_min32(strlen(vid), 8));
  memcpy(product_id, pid, tu_min32(strlen(pid), 16));
  memcpy(product_rev, rev, tu_min32(strlen(rev), 4));
}

// max LUN supported
// we only have the one SD card
extern "C" uint8_t tud_msc_get_maxlun_cb(void) {
  return unitReady ? 1 : 0;
}

// return writable status
// on platform supporting write protect switch, could do that here.
// otherwise this is not actually needed
extern "C" bool tud_msc_is_writable_cb (uint8_t lun)
{
  (void) lun;
  return unitReady;
}

// see https://www.seagate.com/files/staticfiles/support/docs/manual/Interface%20manuals/100293068j.pdf pg 221
extern "C" bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
  (void) lun;
  (void) power_condition;

  if (load_eject)  {
    if (start) {
      // load disk storage
      // do nothing as we started "loaded"
    } else {
      // unmount/eject
      // no further mass storage allowed until reset.
      logmsg("Eject request received. Unmounting.");
      SD.card()->syncDevice();
      unitReady = false;
    }
  }

  return true;
}

// return true if we are ready to service reads/writes
extern "C" bool tud_msc_test_unit_ready_cb(uint8_t lun) {
  (void) lun;

  return unitReady;
}

// return size in blocks and block size
extern "C" void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count,
                         uint16_t *block_size) {
  (void) lun;

  *block_count = unitReady ? (SD.card()->sectorCount()) : 0;
  *block_size = SD_SECTOR_SIZE;
}

// Callback invoked when received an SCSI command not in built-in list (below) which have their own callbacks
// - READ_CAPACITY10, READ_FORMAT_CAPACITY, INQUIRY, MODE_SENSE6, REQUEST_SENSE, READ10 and WRITE10
extern "C" int32_t tud_msc_scsi_cb(uint8_t lun, const uint8_t scsi_cmd[16], void *buffer,
                        uint16_t bufsize) {

  const void *response = NULL;
  uint16_t resplen = 0;

  switch (scsi_cmd[0]) {
  case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
    // Host is about to read/write etc ... better not to disconnect disk
    resplen = 0;
    break;

  default:
    // Set Sense = Invalid Command Operation
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);

    // negative means error -> tinyusb could stall and/or response with failed status
    resplen = -1;
    break;
  }

  // return len must not larger than bufsize
  if (resplen > bufsize) {
    resplen = bufsize;
  }

  // copy response to stack's buffer if any
  if (response && resplen) {
    memcpy(buffer, response, resplen);
  }

  return resplen;
}

// Callback invoked when received READ10 command.
// Copy disk's data to buffer (up to bufsize) and return number of copied bytes (must be multiple of block size)
extern "C" int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, 
                            void* buffer, uint32_t bufsize)
{
  (void) lun;

  bool rc = SD.card()->readSectors(lba, (uint8_t*) buffer, bufsize/SD_SECTOR_SIZE);

  // only blink fast on reads; writes will override this
  if (LEDMode == LED_SOLIDON)
    LEDMode = LED_BLINK_FAST;
    
 /*
 // debug check: only needed if changed the TUD MSC buffer, but did not recompile tinyusb  code
 if (bufsize < SD_SECTOR_SIZE)
    logmsg ("ERROR: USB MSC CFG_TUD_MSC_EP_BUFSIZE (",bufsize,") is smaller than SD card sector size! Card reader will not work.");

  if (bufsize % SD_SECTOR_SIZE)
    logmsg ("ERROR: USB MSC CFG_TUD_MSC_EP_BUFSIZE (",bufsize,") is not a multiple of SD card sector size! Card reader will not work.");   
  */

  return rc ? bufsize : -1;
}

// Callback invoked when receive WRITE10 command.
// Process data in buffer to disk's storage and return number of written bytes (must be multiple of block size)
extern "C" int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           uint8_t *buffer, uint32_t bufsize) {
  (void) lun;

  bool rc = SD.card()->writeSectors(lba, buffer, bufsize/SD_SECTOR_SIZE);

  // always slow blink
  LEDMode = LED_BLINK_SLOW;

  return rc ? bufsize : -1;
}

// Callback invoked when WRITE10 command is completed (status received and accepted by host).
// used to flush any pending cache to storage
extern "C" void tud_msc_write10_complete_cb(uint8_t lun) {
  (void) lun;
  
  SD.card()->syncDevice();
}

#endif