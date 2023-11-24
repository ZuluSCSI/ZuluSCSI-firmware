#include "ZuluSCSI_platform.h"
#include "ZuluSCSI_log.h"
#include <SdFat.h>
#include "msc.h"

/*
TODO:

* how do we enter cardreader mode (and do we return?)
** need to avoid cases where we are using a data cable to power a scsi device attached to something w/o term power. 
** maybe watch rst or other scsi lines for tells we are attached to a machine
* handle card removal?
* case where card not present
* how do we fix the tinyusb buffer without including sources??
** though if we do, we can perhaps repurpose static buffers for other functions
* network buffer has been reduced
** original device has 32K
** need around 10k of free (nonstatic) RAM for stack/heap/dynamic use
*/

extern "C" bool tud_msc_set_sense(uint8_t lun, uint8_t sense_key, uint8_t add_sense_code, uint8_t add_sense_qualifier);

#define SD_SECTOR_SIZE 512

#if CFG_TUD_MSC_EP_BUFSIZE < SD_SECTOR_SIZE
  #error "CFG_TUD_MSC_EP_BUFSIZE is too small! needs to be at least 512  (SD_SECTOR_SIZE)"
#endif

extern SdFs SD;

// usb framework checks this func exists for mass storage config
void __USBInstallMassStorage() { }

// globals
byte blin;
bool unit_ready = false;
uint32_t sd_block_count;

// does not return currently
void cr_run() {

  LED_ON();
  delay(50);
  LED_OFF();

  logmsg("SD card reader...");
  logmsg("Buff size: ", CFG_TUD_MSC_EP_BUFSIZE);
  // Size in blocks (512 bytes)
  sd_block_count = SD.card()->sectorCount();
  platform_reset_watchdog();

  // MSC is ready for read/write
  logmsg("blocks: ", sd_block_count);

  unit_ready = true;
  Serial.println("ready");
 
  // steady state operation
  while(1) {
	  if (blin == 1) { // fast flicker for reads
      LED_ON();
      delay(30);
	  } else if (blin == 2) { // slow flicker for writes
      LED_ON();
      delay(100);
    } /*else {
         
      delay(100);
    }*/
    
	  blin = 0;
	  LED_OFF();
    platform_reset_watchdog();
    delay(30);
  }
}

// Invoked when received SCSI_CMD_INQUIRY
// Application fill vendor id, product id and revision with string up to 8, 16,
// 4 characters respectively
extern "C" void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8],
                        uint8_t product_id[16], uint8_t product_rev[4]) {

  const char vid[] = "ZuluSCSI";
  const char pid[] = "Pico"; // could use strings from the platform, but they are too long
  const char rev[] =  "1.0";

  memcpy(vendor_id, vid, tu_min32(strlen(vid), 8));
  memcpy(product_id, pid, tu_min32(strlen(pid), 16));
  memcpy(product_rev, rev, tu_min32(strlen(rev), 4));
}

extern "C" uint8_t tud_msc_get_maxlun_cb(void) {
  return 1;
}

extern "C" bool tud_msc_is_writable_cb (uint8_t lun)
{
  (void) lun;
  return true;
}

// TODO; sync sd on eject?
extern "C" bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
  (void) lun;
  (void) power_condition;

  if ( load_eject )
  {
    if (start)
    {
      // load disk storage
    }else
    {
      unit_ready = false;
      // unload disk storage
      // we could return to Zulu proper.... may be needed, for UI reasons
    }
  }

  return true;
}

extern "C" bool tud_msc_test_unit_ready_cb(uint8_t lun) {
  (void) lun;
  //todo?
  //tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3a, 0x00);
    
  return unit_ready;
}

extern "C" void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count,
                         uint16_t *block_size) {
  (void) lun;

  *block_count = sd_block_count;
  *block_size = SD_SECTOR_SIZE;
}

// Callback invoked when received an SCSI command not in built-in list below
// - READ_CAPACITY10, READ_FORMAT_CAPACITY, INQUIRY, MODE_SENSE6, REQUEST_SENSE
// - READ10 and WRITE10 has their own callbacks
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

    // negative means error -> tinyusb could stall and/or response with failed
    // status
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
// Copy disk's data to buffer (up to bufsize) and
// return number of copied bytes (must be multiple of block size)
extern "C" int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize)
{
  (void) lun;
  bool rc;
  
  rc = SD.card()->readSectors(lba, (uint8_t*) buffer, bufsize/SD_SECTOR_SIZE);

  if (!blin)
    blin =  1;
    
 /*if (bufsize < SD_SECTOR_SIZE)
    logmsg ("Bufsize is smaller than SD card sector!!!!!!! probably won't work right.");*/

  //logmsg("read ",lba," = ", rc ,", ", bufsize);
 
  return rc ? bufsize : -1;
}

// Callback invoked when received WRITE10 command.
// Process data in buffer to disk's storage and 
// return number of written bytes (must be multiple of block size)
extern "C" int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           uint8_t *buffer, uint32_t bufsize) {
  (void) lun;
  bool rc;
  
  rc = SD.card()->writeSectors(lba, buffer, bufsize/SD_SECTOR_SIZE);

  blin =  2;

  return rc ? bufsize : -1;
}

// Callback invoked when WRITE10 command is completed (status received and accepted by host).
// used to flush any pending cache.
extern "C" void tud_msc_write10_complete_cb(uint8_t lun) {
  (void) lun;
  
  SD.card()->syncDevice();
}
