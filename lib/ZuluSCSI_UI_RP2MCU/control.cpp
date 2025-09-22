/**
 * Copyright (c) 2025 Guy Taylor
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
**/

#if defined(CONTROL_BOARD)

#include "ZuluSCSI_platform.h"
#include "ZuluSCSI_log.h"
#include "ZuluSCSI_disk.h"
#include "ZuluSCSI_settings.h"
#include <ZuluSCSI_buffer_control.h>
#include <minIni.h>
#include <scsi2sd.h>

#include "SystemMode.h"
#include "control.h"
#include "cache.h"
#include "control_global.h"
#include "Screen.h"
#include "MessageBox.h"
#include "SplashScreen.h"
#include "CopyScreen.h"
#include "InitiatorMainScreen.h"
#include "ScreenSaver.h"
#include "UISDNavigator.h"

#define TOTAL_CONTROL_BOARD_BUTTONS 3

#define SCREEN_WIDTH 128 // OLED display width, in pixels
#define SCREEN_HEIGHT 64 // OLED display height, in pixels
#define OLED_RESET     -1 // Reset pin # (or -1 if sharing Arduino reset pin)
#define SCREEN_ADDRESS 0x3C ///< See datasheet for Address; 0x3D for 128x64, 0x3C for 128x32

extern bool g_sdcard_present;

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

typedef enum
{
    ZULUSCSI_UI_START_SPLASH_VERSION,
    ZULUSCSI_UI_START_SPLASH_SYS_MODE,
    ZULUSCSI_UI_START_DONE
} ZULUSCSI_UI_START;

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
Adafruit_SSD1306 *g_display;
DeviceMap *g_devices = nullptr;

#if ZULUSCSI_RESERVED_SRAM_LEN == 0
Adafruit_SSD1306 allocated_g_display(SCREEN_WIDTH, SCREEN_HEIGHT, &g_wire, OLED_RESET);
static DeviceMap static_g_devices[S2S_MAX_TARGETS];
#endif

Screen *g_activeScreen;

int g_previousIndex = SCREEN_ID_NO_PREVIOUS;

SYSTEM_MODE g_systemMode = SYSTEM_NORMAL;

static ZULUSCSI_UI_START g_uiStart = ZULUSCSI_UI_START_SPLASH_VERSION;
static uint32_t g_uiSplashStartTime = 0;

static struct
{
    bool active = false;
    uint32_t startTime = 0;
    uint32_t remainOpenTime = 0;
    SCREEN_TYPE changeScreenTo = SCREEN_NONE;
    int changeDeviceIdTo = SCREEN_ID_UNINITIALIZED;
} g_uiAutoCloseMsgBox;


int g_pcaAddr = 0x3F;

bool g_controlBoardEnabled = false;
bool hasScreenBeenInitialized = false;
bool hasControlBeenInitialized = false;


/// Helpers

const char *systemModeToString(SYSTEM_MODE systemMode)
{
    switch (systemMode)
    {
        case SYSTEM_NORMAL:
            return "Normal Mode";

        case SYSTEM_INITIATOR:
            return "Initiator Mode";

        default:
            return "Unknown";
    }
}

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
            return "FD";

        case S2S_CFG_ZIP100:
            return "ZP";

        case S2S_CFG_REMOVABLE:
            return "RE";

        case S2S_CFG_SEQUENTIAL:
            return "TP";

        case S2S_CFG_NETWORK:
            return "NE";

        case S2S_CFG_NOT_SET:
            return "NA";

  default:
        return "Unknown";
  }
}


void printDevices()
{
    logmsg("**** PrintDevices()");
    
    int i;
    for (i = 0; i < S2S_MAX_TARGETS; i++)
    {
        logmsg("SCSI : ", i);
        logmsg(" Active:         ", g_devices[i].Active);
        logmsg(" User Folder:    ", g_devices[i].UserFolder);
        logmsg(" Root:           ", g_devices[i].RootFolder);
        logmsg(" Path:           ", g_devices[i].Path);
        logmsg(" LoadedObject:   ", g_devices[i].LoadedObject);
        logmsg(" Filename:       ", g_devices[i].Filename);
        logmsg(" Type:           ", g_devices[i].DeviceType);
        logmsg(" Size:           ", g_devices[i].Size);
        logmsg(" IsRemovable:    ", g_devices[i].IsRemovable);
        logmsg(" IsRaw:          ", g_devices[i].IsRaw);
        logmsg(" NavObjectType:  ", g_devices[i].NavObjectType);
    }
}

/**
 * \returns false when init is done, true if still in process
 */
static bool splashScreenPoll()
{
    if (g_uiStart != ZULUSCSI_UI_START_DONE)
    {
        if (g_uiStart == ZULUSCSI_UI_START_SPLASH_VERSION && (uint32_t)(millis() - g_uiSplashStartTime) > 1500)
        {
            _splashScreen->setBannerText(systemModeToString(g_systemMode));
            changeScreen(SCREEN_SPLASH, SCREEN_ID_NO_PREVIOUS);
            g_uiStart = ZULUSCSI_UI_START_SPLASH_SYS_MODE;
            g_uiSplashStartTime = millis();
        }
        else if (g_uiStart == ZULUSCSI_UI_START_SPLASH_SYS_MODE && (uint32_t)(millis() - g_uiSplashStartTime) > 1000)
        {
            g_uiStart = ZULUSCSI_UI_START_DONE;

            if (!g_sdcard_present)
            {
                switch(g_systemMode)
                {
                    case SYSTEM_NORMAL:
                        changeScreen(SCREEN_MAIN, SCREEN_ID_NO_PREVIOUS);
                        g_activeScreen->tick();
                        break;

                    case SYSTEM_INITIATOR:
                        _messageBox->setBlockingMode(true);
                        _messageBox->setText("-- Warning --", "MSC Mode not", "supported yet");
                        changeScreen(MESSAGE_BOX, SCREEN_ID_NO_PREVIOUS);
                        g_activeScreen->tick();
                        break;
                }
            }
            else if(g_systemMode == SYSTEM_NORMAL)
            {
                changeScreen(SCREEN_MAIN, SCREEN_ID_NO_PREVIOUS);
                g_activeScreen->tick();
            }

        }
        return true;
    }
    return false;
}

void deferredMessageBoxClose(uint32_t open_ms, SCREEN_TYPE screen, int deviceId)
{
    if (open_ms == 0)
        changeScreen(screen, deviceId);
    else
    {
        g_uiAutoCloseMsgBox.active = true;
        g_uiAutoCloseMsgBox.changeScreenTo = screen;
        g_uiAutoCloseMsgBox.changeDeviceIdTo = deviceId;
        g_uiAutoCloseMsgBox.remainOpenTime = open_ms;
        g_uiAutoCloseMsgBox.startTime = millis();
    }
}

/**
 * Check if the message box should be closed
 */
static void pollMessageBoxClose()
{
    if (g_uiAutoCloseMsgBox.active && (uint32_t)(millis() - g_uiAutoCloseMsgBox.startTime) > g_uiAutoCloseMsgBox.remainOpenTime)
    {
        g_uiAutoCloseMsgBox.active = false;
        changeScreen(g_uiAutoCloseMsgBox.changeScreenTo, g_uiAutoCloseMsgBox.changeDeviceIdTo);
    }
}

/**
 * poll various UI timers
 */
static void pollUI()
{
    pollMessageBoxClose();
}

/**
 * Skip changeScreen if splash screen is still visible
 * Otherwise change screen if the same screen isn't already loaded or if force_change is true
 * \param screen screen type to change 
 * \param deviceId the device ID
 * \param force_change don't check if the screen is already loaded
 * \returns true if the splash screen is still on going
 */
static bool deferredChangeScreen(SCREEN_TYPE screen, int deviceId, bool force_change = false)
{
    if (splashScreenPoll())
    {
        return true;
    }
    else if (force_change || g_activeScreen->screenType() != screen || g_activeScreen->getOriginalIndex() != deviceId)
    {
        changeScreen(screen, deviceId);
    }
    return false;
}

/**
 * Stops splash screen and then change screen
 * \param screen screen type to change
 * \param deviceId the device ID
 * \returns true if the screen was changed, false if the screen was already loaded
 */
static void overrideSplashScreenLoad(SCREEN_TYPE screen, int deviceId)
{
    g_uiStart = ZULUSCSI_UI_START_DONE;
    changeScreen(screen, deviceId);
}


bool loadImageDeferred(uint8_t id, const char* next_filename, SCREEN_TYPE returnScreen, int returnIndex)
{
    _messageBox->setReturnScreen(returnScreen);
    _messageBox->setReturnConditionPendingLoadComplete();
        
    if (strlen(next_filename) > (MAX_PATH_LEN - 1))
    {
        _messageBox->setText("-- Warning --", "Filepath", "too long...");
        changeScreen(MESSAGE_BOX, returnIndex);
        return false;
    }
    else
    {
        _messageBox->setText("-- Info --", "Loading", "Image...");
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

bool initControlBoardI2C()
{
  g_wire.beginTransmission(g_pcaAddr);
  uint8_t  res = g_wire.endTransmission();
  return res == 0;
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

uint8_t getBrightness()
{
    scsi_system_settings_t *cfg = g_scsi_settings.getSystem();
    return cfg->controlBoardDimDisplay ? 1 : 0x8f;
}

void enableScreen(bool state)
{
    g_display->ssd1306_command(SSD1306_SETCONTRAST);
    uint8_t brightness = getBrightness();
    g_display->ssd1306_command(state ? brightness : 0);
}

bool initScreenHardware()
{
    g_wire.setClock(100000);

    // Setting the drive strength seems to help the I2C bus with the Pico W controller and the controller OLED display
    // to communicate and handshake properly
    gpio_set_drive_strength(GPIO_I2C_SCL, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(GPIO_I2C_SDA, GPIO_DRIVE_STRENGTH_12MA);

    g_wire.begin();
    dbgmsg("Checking for I2C Display");
    g_wire.beginTransmission(SCREEN_ADDRESS);
    if (g_wire.endTransmission() != 0)
    {
        dbgmsg("Could not find Display on I2C bus");
        g_wire.end();
        return false;
    }
#if ZULUSCSI_RESERVED_SRAM_LEN > 0
    uint8_t *object_addr = reserve_buffer_align(sizeof(Adafruit_SSD1306), 4);
    g_display = new (object_addr) Adafruit_SSD1306(SCREEN_WIDTH, SCREEN_HEIGHT, &g_wire, OLED_RESET);
#else
    g_display = &allocated_g_display;
#endif


    // SSD1306_SWITCHCAPVCC = generate display voltage from 3.3V internally
    if(!g_display->begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) 
    {
        logmsg("Failed to initialize Display. Disabling Control Board functionality");

        // Disable Control board as it couldn't be initalized
        g_controlBoardEnabled = false;

        return false;
    }
        // Clear the buffer
    g_display->clearDisplay();
    g_display->display();

    return true;
}

bool initControlBoardHardware()
{
    // System clock may have changed
    g_wire.end();
    g_wire.setClock(100000);
    g_wire.begin();

    initScreens();

    scsi_system_settings_t *cfg = g_scsi_settings.getSystem();

    g_display->setTextWrap(false);
#if defined(ZULUSCSI_BLASTER) 
    if (cfg->controlBoardFlipDisplay || !gpio_get(GPIO_INT))
#else
    if (cfg->controlBoardFlipDisplay)
#endif
    {
        g_display->setRotation(2);
    }

    g_reverseRotary = !cfg->controlBoardReverseRotary;

    if (!initControlBoardI2C())
    {
        logmsg("Failed to init Control Board Input I2C chip. Disabling Control Board functionality");

        // Disable Control board as it couldn't be initalized
        g_controlBoardEnabled = false;

        g_display->clearDisplay();
        g_display->setCursor(0,0);             
        g_display->print(F("ZuluSCSI Control"));
        g_display->drawLine(0,10,127,10, 1);
        g_display->setCursor(0,16);             
        g_display->print(F("Failed to Init I2C chip"));
        g_display->display(); 

        return false;
    }

    enableScreen(true);

    g_controlBoardEnabled = true;
    return true;
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

bool initiatorInitialized = false;

void startInitiator(uint8_t initiatorId)
{
    if (!initiatorInitialized)
    {
        initiatorInitialized = true;

        int i;
        for (i=0;i<S2S_MAX_TARGETS;i++)
        {
            DeviceMap *deviceMap = &g_devices[i];
            deviceMap->InitiatorDriveStatus = i == initiatorId ? INITIATOR_DRIVE_HOST : INITIATOR_DRIVE_UNKNOWN;
        }
    }
    deferredChangeScreen(SCREEN_INITIATOR_MAIN, SCREEN_ID_NO_PREVIOUS);
}

void initUIDisplay()
{
    static bool initDisplayRun = false;
    if (initDisplayRun)
        return;
    else
        initDisplayRun = true;

    if (g_scsi_initiator)
    {
      g_systemMode = SYSTEM_INITIATOR;  
    }
    
    hasScreenBeenInitialized = initScreenHardware();
}

// This clear the list of devices, used at startup and on card removal
void initDevices()
{
#if ZULUSCSI_RESERVED_SRAM_LEN > 0
    if (g_devices == nullptr)
    {
        g_devices = (DeviceMap*) reserve_buffer_align(sizeof(DeviceMap) * S2S_MAX_TARGETS, 4);
    }
#else
    g_devices = static_g_devices;
#endif

    int i;
    for (i = 0; i < S2S_MAX_TARGETS; i++)
    {
        g_devices[i].Active = false;
        g_devices[i].UserFolder = false;
        g_devices[i].NavObjectType = NAV_OBJECT_NONE;
        strcpy(g_devices[i].RootFolder, "");
        strcpy(g_devices[i].LoadedObject, "");
        strcpy(g_devices[i].Path, g_devices[i].RootFolder);

        g_devices[i].DeviceType = S2S_CFG_NOT_SET;
        strcpy(g_devices[i].Filename, "");
        g_devices[i].Size = 0;
        g_devices[i].IsRemovable = false;
        g_devices[i].IsRaw = false;

        // UI Runtime
        g_devices[i].BrowseScreenType = SCREEN_BROWSETYPE_FOLDERS;
    }
}

void initUIPostSDInit(bool sd_card_present)
{
    if (!hasScreenBeenInitialized || hasControlBeenInitialized)
    {
        return;
    }
    hasControlBeenInitialized = true;
    if (initControlBoardHardware())
    {
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

        _splashScreen->setBannerText(g_log_short_firmwareversion);
        changeScreen(SCREEN_SPLASH, SCREEN_ID_NO_PREVIOUS);
        g_activeScreen->tick();
        g_uiStart = ZULUSCSI_UI_START_SPLASH_VERSION;
        g_uiSplashStartTime = millis();
    }

    logmsg("Control Board is ", g_controlBoardEnabled?"Enabled.":"Disabled.");
}

void updateRotary(int dir)
{
    if (g_reverseRotary)
    {
        dir = -dir; // inverter with my encoder
    }

    if (g_activeScreen != NULL)
    {
        if (dir != 0)
        {
            g_userInputDetected = true;
        }
        if (!g_screenSaverActive)
        {
            g_activeScreen->rotaryChange(dir);
        }
    }
}


int g_fileCount;

void scanFileCallback(int count, const char *file, const char *path, u_int64_t size, NAV_OBJECT_TYPE type, const char *cueFile)
{
    g_fileCount = count;
}

void scanForNestedFolders()
{
    _messageBox->setText("-- Busy --", "Scanning", "Files...");
    changeScreen(MESSAGE_BOX, -1);
    _messageBox->tick(); // During boot, there is no loop, so manually trigger the tick, to draw the screen

    int i;
    for (i = 0; i < S2S_MAX_TARGETS; i++)
    {
        DeviceMap *deviceMap = &g_devices[i];

        deviceMap->BrowseScreenType = SCREEN_BROWSETYPE_FOLDERS;
        deviceMap->TotalFlatFiles = 0;

        switch(deviceMap->BrowseMethod)
        {
            case BROWSE_METHOD_NOT_BROWSABLE:
                break;

            case BROWSE_METHOD_IMDDIR:
            {
                bool hasDirs = false;
                SDNavScanFilesRecursive.ScanFilesRecursive(deviceMap->RootFolder, hasDirs, scanFileCallback); 
                deviceMap->HasDirs = hasDirs;
                deviceMap->TotalFlatFiles = g_fileCount+1;

                if (!deviceMap->HasDirs)
                {
                    // default to flat browser if no sub folders
                    deviceMap->BrowseScreenType = SCREEN_BROWSETYPE_FLAT;
                }

                break;
            }
            case BROWSE_METHOD_IMGX:
                // No caching needed
                break;

            case BROWSE_METHOD_USE_PREFIX:
                // Assumption: file will be in the root folder (Dir and Dir(0..9) appear broken)
                // TODO - not implemented yet but is it needed? would seem like a low number of images for this style?
                break;
        }
    }
}

/////// THIS IS WHERE WE CAN INIT, THE DEVICE LIST IS VALID HERE
// Called on sd remove and Device Chnages
void stateChange()
{
    // printDevices();

    g_userInputDetected = true;  // Something changed, so assume the user did interact

    sendSDCardStateChangedToScreens(g_sdAvailable);

    if (g_sdAvailable)
    {
        scsi_system_settings_t *cfg = g_scsi_settings.getSystem();
        g_cacheActive = cfg->controlBoardCache;

        if (g_cacheActive)
        {
            buildCache();
        }
        else
        {
            clearCacheData();

            scanForNestedFolders();
        }

        enableScreen(true); // In case there was a brightness level change
    }

    if (g_uiStart != ZULUSCSI_UI_START_DONE)
        return;

    switch(g_systemMode)
    {
        case SYSTEM_NORMAL:
            changeScreen(SCREEN_MAIN, SCREEN_ID_NO_PREVIOUS);
            g_activeScreen->tick();
            break;
            
        case SYSTEM_INITIATOR:
            _messageBox->setBlockingMode(true);
            _messageBox->setText("-- Warning --", "MSC Mode not", "supported yet");
            changeScreen(MESSAGE_BOX, SCREEN_ID_NO_PREVIOUS);
            g_activeScreen->tick();
            break;
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
    if (!g_controlBoardEnabled)
    {
        return;
    }

    stateChange();
}



void safeCopyString(const char *src, char *dst, uint8_t size)
{
    int i, co = 0;;
    bool zeros = true;
    for (i=0;i<size;i++)
    {

        bool skip = false;
        if (zeros)
        {
            if (src[i] == 32)
            {
                skip = true;
            }
            else
            {
                zeros = false;
            }
        }
        if (!skip)
        {
            dst[co++] = src[i];
        }
    }
    dst[co] =0;

    for (i=co;i>=0;i--)
    {
        if (dst[i] == 0 || dst[i] == 32)
        {
            dst[i] = 0;
        }
        else
        {
            break;
        }
    }
}


// This is the 2nd pass of setting device info (Active/UserFolder/RootFolder will already be set from 1st pass)
void patchDevice(uint8_t target_idx)
{
    image_config_t &img = g_DiskImages[target_idx];

    DeviceMap &map = g_devices[target_idx];

    strcpy(map.Filename, img.current_image);
    map.Size = img.file.size();
    map.IsRaw = img.file.isRaw();
    map.IsRom = img.file.isRom();
    map.IsWritable =  img.file.isWritable();
    
    map.Active = img.file.isOpen();
    map.BrowseMethod = BROWSE_METHOD_NOT_BROWSABLE;

    const S2S_TargetCfg* cfg = s2s_getConfigByIndex(target_idx);
    if (map.Active)
    {
        // 1. A bincue will have set this via binCueInUse(), for all others set it to the filename
        if (strcmp(map.LoadedObject, "") == 0)
        {
            strcpy(map.LoadedObject, map.Filename);
        }
                    
        // 2.  bincue will have set this via binCueInUse(), for all others default to a normal file
        if (map.NavObjectType == NAV_OBJECT_NONE)
        {
            map.NavObjectType = NAV_OBJECT_FILE;
        }
        
        map.DeviceType = (S2S_CFG_TYPE)cfg->deviceType;
        map.IsRemovable = isTypeRemovable((S2S_CFG_TYPE)cfg->deviceType);

        map.LBA = img.get_capacity_lba();
        map.BytesPerSector = cfg->bytesPerSector;
        map.SectorsPerTrack = cfg->sectorsPerTrack;
        map.HeadsPerCylinder = cfg->headsPerCylinder;

        map.Quirks = cfg->quirks;
        safeCopyString(cfg->vendor, map.Vendor, 8);
        safeCopyString(cfg->prodId, map.ProdId, 16);
        safeCopyString(cfg->revision, map.Revision, 4);
        safeCopyString(cfg->serial, map.Serial, 16);

        if (!map.IsRom && !map.IsRaw)
        {
            if (img.image_directory)
            {
                if (map.IsRemovable)
                {
                    map.BrowseMethod = BROWSE_METHOD_IMDDIR;
                }
            }
            else if (img.use_prefix)
            {
                if (map.IsRemovable)
                {
                    map.BrowseMethod = BROWSE_METHOD_USE_PREFIX;
                }
            }
            else
            {
                map.BrowseMethod = BROWSE_METHOD_IMGX;
            
                // MaxImgX
                char filename[MAX_PATH_LEN];
                char section[6] = "SCSI0";
                section[4] = scsiEncodeID(target_idx);
                int j;
                for (j=0;j<=IMAGE_INDEX_MAX;j++)
                {
                    char key[5] = "IMG0";
                    key[3] = '0' + j;

                    ini_gets(section, key, "", filename, MAX_PATH_LEN, CONFIGFILE);
                    if (filename[0] == '\0')
                    {
                        break;
                    }
                }
                map.MaxImgX = j;
            }
        }
    }
    else
    {
        map.DeviceType = S2S_CFG_NOT_SET;
        map.IsRemovable = false;
    }

    /*
    // ASSUMPTION: isOpen() is a good indictor of if a SCSI ID is active

    logmsg("*** g_DiskImages: ", target_idx);

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

    
    if (cfg->scsiId & S2S_CFG_TARGET_ENABLED)
    {
        logmsg("  ***** IS ENABLED!");
    }
    if (cfg && (cfg->scsiId & S2S_CFG_TARGET_ENABLED))
    {
        int capacity_kB = ((uint64_t)cfg->scsiSectors * cfg->bytesPerSector) / 1024;

        if (cfg->deviceType == S2S_CFG_NETWORK)
        {
            logmsg("SCSI ID: ", (int)(cfg->scsiId & 7),
                ", Type: ", typeToShortString((S2S_CFG_TYPE)cfg->deviceType),
                ", Quirks: ", (int)cfg->quirks);
        }
        else
        {
            logmsg("SCSI ID: ", (int)(cfg->scsiId & S2S_CFG_TARGET_ID_BITS),
                ", BlockSize: ", (int)cfg->bytesPerSector,
                ", Type: ", typeToShortString((S2S_CFG_TYPE)cfg->deviceType),
                ", Quirks: ", (int)cfg->quirks,
                ", Size: ", capacity_kB, "kB",
                isTypeRemovable((S2S_CFG_TYPE)cfg->deviceType) ? ", Removable" : ""
                );
        }
    }
    */
}

void patchDevices()
{
    for (uint8_t i = 0; i < S2S_MAX_TARGETS; i++)
    {
        patchDevice(i);
    }
}

//////////////////////////////////////
// Externs called by original ZULU code
//////////////////////////////////////

// When Zulu is determing if drives are folder based, once it works it out, it calls this
// This is used as the 1st pass on setting device info
extern "C" void setFolder(int target_idx, bool userSet, const char *path)
{
    if (!g_controlBoardEnabled)
    {
        return;
    }

    DeviceMap &map = g_devices[target_idx];
    map.Active = true;
    map.UserFolder = userSet;
    strcpy(map.RootFolder, path);
    strcpy(map.Path, path); // Default Cwd to the root
}

void splitPath(const char *full, char *path, char *file)
{
    const char *lastSlash = strrchr(full, '/');
    if (!lastSlash)
    {
        strcpy(path, full);
        strcpy(file, "");
    }
    else
    {
        int len = lastSlash - full;
        strncpy(path, full, len);
        path[len] = 0;

        strcpy(file, lastSlash+1);
    }
}

extern "C" void binCueInUse(int target_idx, const char *foldername) 
{
    DeviceMap &map = g_devices[target_idx];
    map.NavObjectType = NAV_OBJECT_CUE;

    char path[MAX_FILE_PATH];
    char file[MAX_FILE_PATH];

    splitPath(foldername, path, file);

    strcpy(map.Path, path); 
    strcpy(map.LoadedObject, file);
}

// When a card is removed or inserted. If it's removed then clear the device list
extern "C" void sdCardStateChanged(bool sdAvailable)
{
    g_sdAvailable = sdAvailable;

    initDevices();

    if (!g_controlBoardEnabled)
    {
        return;
    }

    if (!sdAvailable) // blank the device map
    {
        patchDevices();
        // printDevices();

        stateChange();
    }
}

// On startup on when a card has finished been reinserted, this is call. Used to call 2nd pass of device setup
extern "C" void scsiReinitComplete()
{
    if (!g_controlBoardEnabled)
    {
        return;
    }

    patchDevices();
    devicesUpdated();
}


bool mscModeMessageDisplayed =false;
extern "C" bool mscMode() 
{
    if (mscModeMessageDisplayed)
    {
        // Check the raw I2C input, any chance, exit
        uint8_t input_byte = getValue();
        if (input_byte != 0xFF)
        {
            volatile uint32_t* scratch0 = (uint32_t *)(WATCHDOG_BASE + WATCHDOG_SCRATCH0_OFFSET);
            *scratch0 = 0;

            _messageBox->setText("-- Info --", "Exiting...", "");
            changeScreen(MESSAGE_BOX, SCREEN_ID_NO_PREVIOUS);
            _messageBox->tick();
    
            return true;
        }
        return false;
    }

    mscModeMessageDisplayed = true;
    _messageBox->setText("-- Info --", "MSC Mode", "Click to exit");
    changeScreen(MESSAGE_BOX, SCREEN_ID_NO_PREVIOUS);
    _messageBox->tick();

    return false;
}

extern "C" void controlLoop()
{
    if (!g_controlBoardEnabled || splashScreenPoll())
    {
        return;
    }

    pollUI();

    // Get the input from the control board
    uint8_t input_byte = getValue();

    processRotaryEncoder(input_byte);
    processButtons(input_byte);

    if (g_activeScreen != NULL)
    {
        if (g_shortPressed[2])
        {
            if (!g_screenSaverActive)
            {
                g_activeScreen->shortRotaryPress();
            }
            g_userInputDetected = true;
        }
        if (g_longPressed[2])
        {
            if (!g_screenSaverActive)
            {
                g_activeScreen->longRotaryPress();
            }
            g_userInputDetected = true;
        }
        if (g_shortPressed[0])
        {
            if (!g_screenSaverActive)
            {
                g_activeScreen->shortUserPress();
            }
            g_userInputDetected = true;
        }
        if (g_longPressed[0])
        {
            if (!g_screenSaverActive)
            {   
                g_activeScreen->longUserPress();
            }
            g_userInputDetected = true;
        }
        if (g_shortPressed[1])
        {
            if (!g_screenSaverActive)
            {
                g_activeScreen->shortEjectPress();
            }
            g_userInputDetected = true;
        }
        if (g_longPressed[1])
        {
            if (!g_screenSaverActive)
            {
                g_activeScreen->longEjectPress();
            }
            g_userInputDetected = true;
        }

        screenSaverLoop();

        if (!g_screenSaverActive)
        {
            if (g_activeScreen != NULL)
            {
                g_activeScreen->tick();
            }
        }
        else
        {
            drawScreenSaver();
        }
    }

    g_userInputDetected = false;
}

// Create 
void UICreateInit(uint64_t blockCount, uint32_t blockSize, const char *filename) 
{
    if (!g_controlBoardEnabled)
    {
        return;
    }

    _copyScreen->TotalRetries = 0;
    _copyScreen->TotalErrors  = 0;

    _copyScreen->DeviceType = 255;
    _copyScreen->BlockCount = blockCount;
    _copyScreen->BlockSize = blockSize;

    _copyScreen->setBannerText("Create Image");
    _copyScreen->setShowInfoText(true);
    _copyScreen->setInfoText(filename);
    _copyScreen->setShowRetriesAndErrors(false);
    overrideSplashScreenLoad(SCREEN_COPY, SCREEN_ID_OTHER);
}

void UICreateProgress(uint32_t blockTime, uint32_t blockCopied) 
{
    if (!g_controlBoardEnabled)
    {
        return;
    }
    _copyScreen->BlockTime = blockTime;
    _copyScreen->BlocksCopied = blockCopied;
    _copyScreen->BlocksInBatch = 1;
    _copyScreen->NeedsProcessing = true;
    _copyScreen->tick();
}

/// Kiosk
void UIKioskCopyInit(uint8_t deviceIndex, uint8_t totalDevices, uint64_t blockCount, uint32_t blockSize, const char *filename)
{
    if (!g_controlBoardEnabled)
    {
        return;
    }
    _copyScreen->TotalRetries = 0;
    _copyScreen->TotalErrors  = 0;

    _copyScreen->DeviceType = 255;
    _copyScreen->BlockCount = blockCount;
    _copyScreen->BlockSize = blockSize;


    char t[10];
    char banner[MAX_PATH_LEN];
    
    strcpy(banner, "Kiosk Restore (");
    itoa(deviceIndex, t, 10);
    strcat(banner, t);
    strcat(banner, "/");
    itoa(totalDevices, t, 10);
    strcat(banner, t);
    strcat(banner, ")");

    _copyScreen->setBannerText(banner);
    
    _copyScreen->setShowInfoText(true);
    _copyScreen->setInfoText(filename);
    _copyScreen->setShowRetriesAndErrors(false);

    overrideSplashScreenLoad(SCREEN_COPY, SCREEN_ID_OTHER);
}

void UIKioskCopyProgress(uint32_t blockTime, uint32_t blockCopied)
{
    if (!g_controlBoardEnabled)
    {
        return;
    }
    _copyScreen->BlockTime = blockTime;
    _copyScreen->BlocksCopied = blockCopied;
    _copyScreen->BlocksInBatch = 1;
    _copyScreen->NeedsProcessing = true;

    _copyScreen->tick();
}

/// Rom copy
void UIRomCopyInit(uint8_t deviceId, S2S_CFG_TYPE deviceType, uint64_t blockCount, uint32_t blockSize, const char *filename)
{
    if (!g_controlBoardEnabled)
    {
        return;
    }
    _copyScreen->TotalRetries = 0;
    _copyScreen->TotalErrors  = 0;

    _copyScreen->DeviceType = deviceType;
    _copyScreen->BlockCount = blockCount;
    _copyScreen->BlockSize = blockSize;

    _copyScreen->setBannerText("Flashing ROM");
     _copyScreen->setShowInfoText(true);
    _copyScreen->setInfoText(&filename[1]);
    _copyScreen->setShowRetriesAndErrors(false);
    overrideSplashScreenLoad(SCREEN_COPY, deviceId);
}
void UIRomCopyProgress(uint8_t deviceId, uint32_t blockTime, uint32_t blocksCopied) 
{
    if (!g_controlBoardEnabled)
    {
        return;
    }
    _copyScreen->BlockTime = blockTime;
    _copyScreen->BlocksCopied = blocksCopied;
    _copyScreen->BlocksInBatch = 1;
    _copyScreen->NeedsProcessing = true;

    _copyScreen->tick();
}

/// Initiator
INITIATOR_MODE g_initiatorMode = INITIATOR_SCANNING;
bool g_initiatorMessageToProcess;

void UIInitiatorScanning(uint8_t deviceId, uint8_t initiatorId)
{
    if (!g_controlBoardEnabled)
    {
        return;
    }

    startInitiator(initiatorId);

    g_initiatorMessageToProcess = true;

    DeviceMap *deviceMap = &g_devices[deviceId];

    if (deviceMap->InitiatorDriveStatus == INITIATOR_DRIVE_UNKNOWN || deviceMap->InitiatorDriveStatus == INITIATOR_DRIVE_SCANNED)
    {
        deviceMap->InitiatorDriveStatus = INITIATOR_DRIVE_PROBING;
        deviceMap->TotalRetries = 0;
        deviceMap->TotalErrors  = 0;

        _copyScreen->TotalRetries = 0;
        _copyScreen->TotalErrors  = 0;
    }

    for (uint8_t i = 0 ; i < S2S_MAX_TARGETS; i++)
    {
        if (i != deviceId && g_devices[i].InitiatorDriveStatus == INITIATOR_DRIVE_PROBING)
        {
            g_devices[i].InitiatorDriveStatus = INITIATOR_DRIVE_SCANNED;
        }
    }
    if (!deferredChangeScreen(SCREEN_INITIATOR_MAIN, SCREEN_ID_NO_PREVIOUS))
        _initiatorMainScreen->tick();
}

void UIInitiatorReadCapOk(uint8_t deviceId, S2S_CFG_TYPE deviceType, uint64_t sectorCount, uint32_t sectorSize) 
{
    if (!g_controlBoardEnabled)
    {
        return;
    }

    g_initiatorMessageToProcess = true;

    DeviceMap *deviceMap = &g_devices[deviceId];

    deviceMap->DeviceType = deviceType;
    deviceMap->SectorCount = sectorCount;
    deviceMap->SectorSize = sectorSize;

    _copyScreen->DeviceType = 255;
    _copyScreen->BlockCount = sectorCount;
    _copyScreen->BlockSize = sectorSize;

    if (deviceMap->InitiatorDriveStatus == INITIATOR_DRIVE_SCANNED)
    {
        deviceMap->InitiatorDriveStatus = INITIATOR_DRIVE_CLONABLE;
    }

    _copyScreen->setBannerText("Cloning");
    _copyScreen->setShowRetriesAndErrors(true);
    _copyScreen->setShowInfoText(false);

    deferredChangeScreen(SCREEN_COPY, deviceId, true);
}

void UIInitiatorProgress(uint8_t deviceId, uint32_t blockTime, uint32_t sectorsCopied, uint32_t sectorInBatch) 
{
    if (!g_controlBoardEnabled)
    {
        return;
    }
    _copyScreen->BlockTime = blockTime;
    _copyScreen->BlocksCopied = sectorsCopied;
    _copyScreen->BlocksInBatch = sectorInBatch;
    _copyScreen->NeedsProcessing = true;

    deferredChangeScreen(SCREEN_COPY, deviceId);
}

void UIInitiatorRetry(uint8_t deviceId) 
{
    if (!g_controlBoardEnabled)
    {
        return;
    }

    g_initiatorMessageToProcess = true;

    DeviceMap *deviceMap = &g_devices[deviceId];
    deviceMap->TotalRetries++;
    _copyScreen->TotalRetries++;
    deferredChangeScreen(SCREEN_COPY, deviceId);
}

void UIInitiatorSkippedSector(uint8_t deviceId) 
{
    if (!g_controlBoardEnabled)
    {
        return;
    }

    g_initiatorMessageToProcess = true;

    DeviceMap *deviceMap = &g_devices[deviceId];
    deviceMap->TotalErrors++;
    _copyScreen->TotalErrors++;
    deferredChangeScreen(SCREEN_COPY, deviceId);
}

void UIInitiatorTargetFilename(uint8_t deviceId, char *filename) 
{
    if (!g_controlBoardEnabled)
    {
        return;
    }

    g_initiatorMessageToProcess = true;

    DeviceMap *deviceMap = &g_devices[deviceId];
    strcpy(deviceMap->Filename, filename);
}

void UIInitiatorFailedToTransfer(uint8_t deviceId) 
{
    if (!g_controlBoardEnabled)
    {
        return;
    }

    g_initiatorMessageToProcess = true;
}

void UIInitiatorImagingComplete(uint8_t deviceId) 
{
    if (!g_controlBoardEnabled)
    {
        return;
    }

    g_initiatorMessageToProcess = true;

    DeviceMap *deviceMap = &g_devices[deviceId];

    deviceMap->InitiatorDriveStatus = INITIATOR_DRIVE_CLONED;
    deferredChangeScreen(SCREEN_INITIATOR_MAIN, SCREEN_ID_NO_PREVIOUS, true);
}


#endif
