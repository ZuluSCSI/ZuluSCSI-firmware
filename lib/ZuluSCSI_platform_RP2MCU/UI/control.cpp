#if defined(CONTROL_BOARD) && !defined(ENABLE_AUDIO_OUTPUT_SPDIF)

#include "ZuluSCSI_platform.h"
#include "ZuluSCSI_log.h"
#include "ZuluSCSI_disk.h"
#include "ZuluSCSI_settings.h"
#include <scsi2sd.h>

#include "control.h"
#include "cache.h"
#include "control_global.h"
#include "Screen.h"
#include "MessageBox.h"

#define TOTAL_CONTROL_BOARD_BUTTONS 3

#define SCREEN_WIDTH 128 // OLED display width, in pixels
#define SCREEN_HEIGHT 64 // OLED display height, in pixels
#define OLED_RESET     -1 // Reset pin # (or -1 if sharing Arduino reset pin)
#define SCREEN_ADDRESS 0x3C ///< See datasheet for Address; 0x3D for 128x64, 0x3C for 128x32

enum rotary_direction_t {
    ROTARY_DIR_NONE = 0,
    ROTARY_DIR_CW   = 0x10,
    ROTARY_DIR_CCW  = 0x20
  };

 enum rotatry_state_t {
    ROTARY_TICK_000 = 0,
    ROTARY_LAST_CW_001,
    ROTARY_START_CW_010,
    ROTARY_CONT_CW_011,
    ROTARY_START_CCW_100,
    ROTARY_LAST_CCW_101,
    ROTARY_CONT_CCW_110,
  };

const uint8_t rotary_transition_lut[7][4] = 
{
  // From ROTARY_TICK_000
  {ROTARY_TICK_000,      ROTARY_START_CW_010,   ROTARY_START_CCW_100,  ROTARY_TICK_000},
  // From ROTARY_LAST_CW_001
  {ROTARY_CONT_CW_011,   ROTARY_TICK_000,       ROTARY_LAST_CW_001,    ROTARY_TICK_000 | ROTARY_DIR_CW},
  // From ROTARY_START_CW_001
  {ROTARY_CONT_CW_011,   ROTARY_START_CW_010,   ROTARY_TICK_000,       ROTARY_TICK_000},
  // From ROTARY_CONT_CW_011
  {ROTARY_CONT_CW_011,   ROTARY_START_CW_010,   ROTARY_LAST_CW_001,    ROTARY_TICK_000},
  // From ROTARY_START_CCW_100
  {ROTARY_CONT_CCW_110,  ROTARY_TICK_000,       ROTARY_START_CCW_100,  ROTARY_TICK_000},
  // From ROTARY_LAST_CCW_101
  {ROTARY_CONT_CCW_110,  ROTARY_LAST_CCW_101,   ROTARY_TICK_000,       ROTARY_TICK_000 | ROTARY_DIR_CCW},
  // from ROTARY_CONT_CCW_110
  {ROTARY_CONT_CCW_110,  ROTARY_LAST_CCW_101,   ROTARY_START_CCW_100,  ROTARY_TICK_000},
};

char g_tmpFilename[MAX_PATH_LEN];
char g_tmpFilepath[MAX_PATH_LEN];

// Device state tracking
bool g_sdAvailable = false;
bool g_reverseRotary;

static uint8_t g_rotary_state;
bool g_going_cw;
int g_tick_count;
uint8_t g_number_of_ticks;

bool g_shortPressed[TOTAL_CONTROL_BOARD_BUTTONS];
bool g_longPressed[TOTAL_CONTROL_BOARD_BUTTONS];

long g_button3StartTime[TOTAL_CONTROL_BOARD_BUTTONS];
bool g_button3Press[TOTAL_CONTROL_BOARD_BUTTONS];
bool g_waitingForButton3Release[TOTAL_CONTROL_BOARD_BUTTONS];

static TwoWire g_wire(i2c1, GPIO_I2C_SDA, GPIO_I2C_SCL);
Adafruit_SSD1306 g_display(SCREEN_WIDTH, SCREEN_HEIGHT, &g_wire, OLED_RESET);

DeviceMap g_devices[TOTAL_DEVICES];
Screen *g_activeScreen;

int g_prevousIndex = -1;

int g_pcaAddr = 0x3F;
bool g_deviceExists = false;

bool g_controlBoardInit = false;
bool g_firstInitDevices = true; // For the first lots of SetFolder during start up

/// Helpers

const char* typeToShortString(S2S_CFG_TYPE type) 
{
    switch (type)
    {
        case S2S_CFG_FIXED:
            return "HD";

        case S2S_CFG_OPTICAL:
            return "CD";

        case S2S_CFG_MO:
            return "MO";

        case S2S_CFG_FLOPPY_14MB:
            return "Flop";

        case S2S_CFG_ZIP100:
            return "Zip";

        case S2S_CFG_REMOVABLE:
            return "Rem";

        case S2S_CFG_SEQUENTIAL:
            return "Seq";

        case S2S_CFG_NETWORK:
            return "Net";

        case S2S_CFG_NOT_SET:
            return "N/A";

  default:
        return "Unknown";
  }
}


void printDevices()
{
    logmsg("**** PrintDevices()");
    
    int i;
    for (i=0;i<TOTAL_DEVICES;i++)
    {
        logmsg("  - ", i, "  Active: ", g_devices[i].Active, " user folder ",  g_devices[i].UserFolder,  "  path " , g_devices[i].RootFolder, "  CWD: ", g_devices[i].Path);
        logmsg("    -  Type: ", typeToShortString((S2S_CFG_TYPE)g_devices[i].DeviceType)
            ," CurrentFilename ",  g_devices[i].Filename
            ,"  Size  " , g_devices[i].Size
            ,"  IsRemovable " , g_devices[i].IsRemovable,
             "  IsRaw " , g_devices[i].IsRaw);
    }
}

bool loadImageDeferred(uint8_t id, const char* next_filename, SCREEN_TYPE returnScreen, int returnIndex)
{
    _messageBox.setReturnScreen(returnScreen);
    _messageBox.setReturnConditionPendingLoadComplete();
        
    if (strlen(next_filename) > 63)
    {
        _messageBox.setText("-- Warning --", "Filepath", "too long...");
        changeScreen(MESSAGE_BOX, returnIndex);
        return false;
    }
    else
    {
        _messageBox.setText("-- Info --", "Loading", "Image...");
        changeScreen(MESSAGE_BOX, returnIndex);

        setPendingImageLoad(id, next_filename);
        return true;
    }
}


static bool isTypeRemovable(S2S_CFG_TYPE type)
{
  switch (type)
  {
  case S2S_CFG_OPTICAL:
  case S2S_CFG_MO:
  case S2S_CFG_FLOPPY_14MB:
  case S2S_CFG_ZIP100:
  case S2S_CFG_REMOVABLE:
  case S2S_CFG_SEQUENTIAL:
    return true;
  default:
    return false;
  }
}


extern "C" void initDisplay()
{
    g_wire.setClock(100000);

    // Setting the drive strength seems to help the I2C bus with the Pico W controller and the controller OLED display
    // to communicate and handshake properly
    gpio_set_drive_strength(GPIO_I2C_SCL, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(GPIO_I2C_SDA, GPIO_DRIVE_STRENGTH_12MA);
    
    // SSD1306_SWITCHCAPVCC = generate display voltage from 3.3V internally
    if(!g_display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) 
    {
        logmsg("SSD1306 allocation failed");
    }
    
    g_display.setTextWrap(false);
    g_display.setRotation(2);
   
      // Clear the buffer
    g_display.clearDisplay();
    g_display.display(); 
}

bool checkForDevice()
{
  g_wire.beginTransmission(g_pcaAddr);
  uint8_t  res = g_wire.endTransmission();
  logmsg("inside I2c Init");
  logmsg(" g_wire.endTransmission(); = ");
  logmsg(res);
  g_deviceExists = 0 == res;

  return g_deviceExists;
}

uint8_t getValue() 
{
  uint8_t input_byte = 0xFF;

  g_wire.beginTransmission(g_pcaAddr);
  g_wire.write(0);
  g_wire.endTransmission();

  g_wire.requestFrom(g_pcaAddr, 1);
  while(g_wire.available()) 
  {
    input_byte = g_wire.read();
  }
  
  return input_byte;
}

extern "C" void initControlBoardI2CExpander()
{
    checkForDevice();
    if (!g_deviceExists)
    {
        logmsg("Init bad :(");
    }
}

void checkButton(uint8_t input_byte, int index)
{
    long mcs = millis();
    
    uint8_t mask = 0;
    switch(index)
    {
        case 0:
            mask = EXP_EJECT_PIN;
            break;

        case 1:
            mask = EXP_INSERT_PIN;
            break;

        case 2:
            mask = EXP_ROT_PIN;
            break;

    }

    bool value = !((1 << mask) & input_byte) != 0;

    // check button
    if (!g_button3Press[index])
    {
        if (value == true)
        { 
            g_button3Press[index] = true;
            g_waitingForButton3Release[index] = true;
            g_button3StartTime[index] = mcs;
        }
    }
    else
    {
        if (mcs - g_button3StartTime[index] > 100)
        {
            if (g_waitingForButton3Release[index])
            {
                if (value == false)
                {      
                    g_waitingForButton3Release[index] = false; 
                    g_button3Press[index] = false;
                                 
                    if (mcs - g_button3StartTime[index] > 1000)
                    {
                        g_longPressed[index] = true;
                    }
                    else
                    {
                        g_shortPressed[index] = true;
                    }
                }
            }
        }
    }
}

void processButtons(uint8_t input_byte)
{
    int i;
    for (i=0;i<TOTAL_CONTROL_BOARD_BUTTONS;i++)
    {
        g_shortPressed[i] = false;
        g_longPressed[i] = false;

        checkButton(input_byte, i);
    }
}

void initUI()
{
    initDisplay();

    scsi_system_settings_t *cfg = g_scsi_settings.getSystem();
    g_reverseRotary = !cfg->reverseControlBoardRotary;

    changeScreen(SCREEN_SPLASH, -1);
}

void updateRotary(int dir)
{
    if (g_reverseRotary)
    {
        dir = -dir; // inverter with my encoder
    }

    g_activeScreen->rotaryChange(dir);
}


/////// THIS IS WHERE WE CAN INIT, THE DEVICE LIST IS VALID HERE
// Called on sd remove and Device Chnages
void stateChange()
{
    logmsg("*** stateChange()");
    printDevices();

    sendSDCardStateChangedToScreens(g_sdAvailable);

    if (g_sdAvailable)
    {
        // either boot or valid card change
        scsi_system_settings_t *cfg = g_scsi_settings.getSystem();
        g_cacheActive = cfg->enableControlBoardCache;
        
        if (g_cacheActive)
        {
            buildCache();
        }
        else
        {
            clearCahceData();
        }
        
        changeScreen(SCREEN_MAIN, -1);
    }
    else
    {
        // card removed
        changeScreen(SCREEN_NOSD, -1);
    }
}

void processRotaryEncoder(uint8_t input_byte)
{
    // Rotary
    uint8_t chan_a = 1 & (input_byte >> EXP_ROT_A_PIN);
    uint8_t chan_b = 1 & (input_byte >> EXP_ROT_B_PIN);
    uint8_t rotaryInputState =  chan_b << 1 | chan_a;
    g_rotary_state = rotary_transition_lut[g_rotary_state & 0x0F][rotaryInputState];
    rotary_direction_t rotaryDirection = (rotary_direction_t) (g_rotary_state & 0x30);

    if (g_going_cw) 
    {
        if (rotaryDirection == ROTARY_DIR_CW) 
        {
            if (g_tick_count < g_number_of_ticks - 1) 
            {
                g_tick_count++;
            }    
            else 
            {
                updateRotary(-1);
                g_tick_count = 0;
            }
        } 
        else if (rotaryDirection == ROTARY_DIR_CCW) 
        {
            g_tick_count = 1;
            if (g_tick_count > g_number_of_ticks - 1) 
            {
                updateRotary(1);
            }
            g_going_cw = false;
        }
    } 
    else
    {
        if (rotaryDirection == ROTARY_DIR_CCW) 
        {
            if (g_tick_count < g_number_of_ticks - 1) 
            {
                g_tick_count++;
            }
            else 
            {
                updateRotary(1);
                g_tick_count = 0;
            }
        } 
        else if (rotaryDirection == ROTARY_DIR_CW) 
        {
            g_tick_count = 1;
            if (g_tick_count > g_number_of_ticks - 1) 
            {
                updateRotary(-1);
            }
            g_going_cw = true;
        }
    }
}

// When this is called, the device list is complete, either on startup or on a card swap
void devicesUpdated()
{
    logmsg("*** devicesUpdated()");
    if (!g_controlBoardInit)
    {
        return;
    }
    
    stateChange();
}

// This clear the list of devices, used at startup and on card removal
void initDevices()
{
    logmsg("*** initDevices()");

    int i;
    for (i=0;i<TOTAL_DEVICES;i++)
    {
        g_devices[i].Active = false;
        g_devices[i].UserFolder = false;
        strcpy(g_devices[i].RootFolder, "");
        strcpy(g_devices[i].Path, g_devices[i].RootFolder);

        g_devices[i].DeviceType = S2S_CFG_NOT_SET;
        strcpy(g_devices[i].Filename, "");
        g_devices[i].Size = 0;
        g_devices[i].IsRemovable = false;
        g_devices[i].IsRaw = false;
    }
}

// This is the 2nd pass of setting device info (Active/UserFolder/RootFolder will already be set from 1st pass)
void patchDevice(uint8_t i)
{
    image_config_t &img = g_DiskImages[i];

    DeviceMap &map = g_devices[i];

    strcpy(map.Filename, img.current_image);
    map.Size = img.file.size();
    map.IsRaw = img.file.isRaw();
    map.IsRom = img.file.isRom();
    map.IsWritable =  img.file.isWritable();
    
    if (!map.Active)  // If it wasn't discover in the folder setting phase, it might be a fixed drive or something else, set it here
    {
        map.Active = img.file.isOpen();
    }

    const S2S_TargetCfg* cfg = s2s_getConfigByIndex(i);
    if (cfg && (cfg->scsiId & S2S_CFG_TARGET_ENABLED))
    {
        map.DeviceType = cfg->deviceType;
        map.IsRemovable = isTypeRemovable((S2S_CFG_TYPE)cfg->deviceType);
        map.IsBrowsable = map.IsRemovable && map.UserFolder;
    }
    else
    {
        map.DeviceType = S2S_CFG_NOT_SET;
        map.IsRemovable = false;
        map.IsBrowsable = false;
    }

    /*
    // ASSUMPTION: isOpen() is a good indictor of if a SCSI ID is active

    logmsg("*** g_DiskImages: ", i);

    logmsg("   ejectButton: ", img.ejectButton);
    logmsg("   ejected: ", img.ejected);
    logmsg("   cdrom_events: ", img.cdrom_events);
    logmsg("   reinsert_on_inquiry: ", img.reinsert_on_inquiry);
    logmsg("   reinsert_after_eject: ", img.reinsert_after_eject);
    logmsg("   use_prefix: ", img.use_prefix);
    logmsg("   current_image: ", img.current_image);
    logmsg("   name_from_image: ", img.name_from_image);
    logmsg("   get_capacity_lba(): ", img.get_capacity_lba());
    
    logmsg("   file info: ");
    logmsg("      isOpen() ", img.file.isOpen());
    logmsg("      isWritable() ", img.file.isWritable());
    logmsg("      isRaw() ", img.file.isRaw());
    logmsg("      size() ", img.file.size());

    
    const S2S_TargetCfg* cfg = s2s_getConfigByIndex(i);
    if (cfg && (cfg->scsiId & S2S_CFG_TARGET_ENABLED))
    {
        int capacity_kB = ((uint64_t)cfg->scsiSectors * cfg->bytesPerSector) / 1024;

        if (cfg->deviceType == S2S_CFG_NETWORK)
        {
            logmsg("SCSI ID: ", (int)(cfg->scsiId & 7),
                ", Type: ", TypeToString((S2S_CFG_TYPE)cfg->deviceType),
                ", Quirks: ", (int)cfg->quirks);
        }
        else
        {
            logmsg("SCSI ID: ", (int)(cfg->scsiId & S2S_CFG_TARGET_ID_BITS),
                ", BlockSize: ", (int)cfg->bytesPerSector,
                ", Type: ", TypeToString((S2S_CFG_TYPE)cfg->deviceType),
                ", Quirks: ", (int)cfg->quirks,
                ", Size: ", capacity_kB, "kB",
                IsTypeRemovable((S2S_CFG_TYPE)cfg->deviceType) ? ", Removable" : ""
                );
        }
    }
    */
}

void patchDevices()
{
    logmsg("*** patchDevices()");
    for (uint8_t i = 0; i < S2S_MAX_TARGETS; i++)
    {
        patchDevice(i);
    }
}

//////////////////////////////////////
// Externs called by orignal ZULU code
//////////////////////////////////////

// When Zulu is determing if drives are folder based, once it works it out, it calls this
// This is used as the 1st pass on setting device info
extern "C" void setFolder(int target_idx, bool userSet, const char *path)
{
    if (g_firstInitDevices)
    {
        g_firstInitDevices = false;
        initDevices();
    }

    DeviceMap &map = g_devices[target_idx];
    map.Active = true;
    map.UserFolder = userSet;
    strcpy(map.RootFolder, path);
    strcpy(map.Path, path); // Default Cwd to the root
}

// When a card is removed or inserted. If it's removed then clear the device list
extern "C" void sdCardStateChanged(bool absent)
{
    logmsg("*** sdCardStateChanged()); absent = ", absent);

    g_sdAvailable = !absent;

    if (absent) // blank the device map
    {
        initDevices();

        stateChange();
    }
}

// On startup on when a card has finished been reinserted, this is call. Used to call 2nd pass of device seup
extern "C" void scsiReinitComplete()
{
    logmsg("*** scsiReinitComplete()");
    patchDevices();

    devicesUpdated();
}

extern "C" void controlInit()
{
    logmsg("ControlBoard enabled.");

    initControlBoardI2CExpander();

    g_tick_count = 0;
    g_going_cw = true;
    g_number_of_ticks = 1;
    g_rotary_state = ROTARY_TICK_000; 

    int i;
    for (i=0;i<TOTAL_CONTROL_BOARD_BUTTONS;i++)
    {
        g_button3StartTime[i] = -1;
        g_button3Press[i] = false;
        g_waitingForButton3Release[i] = false;

        g_shortPressed[i] = false;
        g_longPressed[i] = false;
    }
    
    g_controlBoardInit = true;
    
    devicesUpdated();
}

extern "C" void controlLoop()
{
    // Get the input from the control board
    uint8_t input_byte = getValue();

    processRotaryEncoder(input_byte);
    processButtons(input_byte);

    if (g_shortPressed[2])
    {
        g_activeScreen->shortRotaryPress();
    }
    if (g_longPressed[2])
    {
        g_activeScreen->longRotaryPress();
    }
    if (g_shortPressed[0])
    {
        g_activeScreen->shortUserPress();
    }
    if (g_longPressed[0])
    {
        g_activeScreen->longUserPress();
    }
    if (g_shortPressed[1])
    {
        g_activeScreen->shortEjectPress();
    }
    if (g_longPressed[1])
    {
        g_activeScreen->longEjectPress();
    }

    g_activeScreen->tick();
}

#endif