ZuluSCSI™ Firmware
=================

Hard Drive & ISO image files
---------------------
ZuluSCSI uses raw hard drive image files, which are stored on a FAT32 or exFAT-formatted SD card. These are often referred to as "hda" files.

Examples of valid filenames:
* `HD5.hda` or `HD5.img`: hard drive with SCSI ID 5
* `HD20_512.hda`: hard drive with SCSI ID 2, LUN 0, block size 512. Currently, ZuluSCSI does not support multiple LUNs, only LUN 0.
* `CD3.iso`: CD drive with SCSI ID 3. The default CD block size is 2048. This can be overridden by setting the block size manually, eg `CD3_512.iso`

In addition to the simplified filenames style above, the ZuluSCSI firmware also looks for images using the BlueSCSI-style "HDxy_512.hda" filename formatting.

The media type can be set in `zuluscsi.ini`, or directly by the file name prefix.
Supported prefixes are `HD` (hard drive), `CD` (cd-rom), `FD` (floppy), `MO` (magneto-optical), `RE` (generic removeable media), `TP` (sequential tape drive).

CD-ROM images in BIN/CUE format
-------------------------------
The `.iso` format for CD images only supports data track.
For audio and mixed mode CDs, two files are needed: `.bin` with data and `.cue` with the list of tracks.

To use a BIN/CUE image with ZuluSCSI, name both files with the same part before the extension.
For example `CD3.bin` and `CD3.cue`.
The cue file contains the original file name, but it doesn't matter for ZuluSCSI.

If the image consists of one `.cue` file and multiple `.bin` files, they need to be placed in a separate subfolder.
For example, create `CD3` folder, then `MyGame` subfolder and put the `.cue` and `.bin` files there.
The `.bin` file names must then match the names specified in the `.cue` file.

Supported track types are `AUDIO`, `MODE1/2048` and `MODE1/2352`.

Creating new image files
------------------------
Empty image files can be created using operating system tools:

* Windows: `fsutil file createnew HD1.img 1073741824` (1 GB)
* Linux: `truncate -s 1G HD1.img`
* Mac OS X: `mkfile -n 1g HD1.img`

If you need to use image files larger than 4GB, you _must_ use an exFAT-formatted SD card, as the FAT32 filesystem does not support files larger than 4,294,967,295 bytes (4GB-1 byte).

ZuluSCSI firmware can also create image files itself.
To do this, create a text file with filename such as `Create 1024M HD40.txt`.
The special filename must start with "Create" and be followed by file size and the name of resulting image file.
The file will be created next time the SD card is inserted.
The status LED will flash rapidly while image file generation is in progress.

Log files and error indications
-------------------------------
Log messages are stored in `zululog.txt`, which is cleared on every boot.
Normally only basic initialization information is stored, but switching the `DBG` DIP switch on will cause every SCSI command to be logged, once the board is power cycled.

The indicator LED will normally report disk access.
It also reports following status conditions:

- 1 fast blink on boot: Image file loaded successfully
- 3 fast blinks: No images found on SD card
- 5 fast blinks: SD card not detected
- Continuous morse pattern: firmware crashed, morse code indicates crash location

In crashes the firmware will also attempt to save information into `zuluerr.txt`.

Configuration file
------------------
Optional configuration can be stored in `zuluscsi.ini`.
If image file is found but configuration is missing, a default configuration is used.

Example config file is available here: [zuluscsi.ini](zuluscsi.ini).

Performance
-----------
Performance information for the various ZuluSCSI hardware models is [documented separately, here](https://github.com/ZuluSCSI/ZuluSCSI-firmware/wiki/Performance)

Hotplugging
-----------
The firmware supports hot-plug removal and reinsertion of SD card.
The status led will blink continuously when card is not present, then blink once when card is reinserted successfully.

It will depend on the host system whether it gets confused by hotplugging.
Any IO requests issued when card is removed will be timeouted.

Programming & bootloader
------------------------
For ZuluSCSI Blaster and RP2040-based boards, the USB programming uses `.uf2` format file that can be copied to the virtual USB drive that shows up in bootloader mode.

- There is a custom bootloader that loads new firmware from SD card on boot.
- Firmware update takes about 1 second, during which the LED will flash rapidly.
- When successful, the bootloader removes the update file and continues to main firmware.
- On ZuluSCSI Blaster and RP2040 boards, there is a "BOOTLDR" momentary-contact switch, which can be held down at initial power-on, to enable .uf2 firmware to be loaded.


For ZuluSCSI V1.1 and V1.2:

- Alternatively, ZuluSCSI V1.x can be programmed using USB connection in DFU mode by setting DIP switch 4.
- For ZuluSCSI V1.2 boards, there is a "BOOTLDR" momentary-contact switch, which can be held down at initial power-on, to enable DFU mode, needed for firmware recovery mode.
- The necessary programmer utility for Windows can be downloaded from [GD32 website](http://www.gd32mcu.com/en/download?kw=dfu&lan=en). On Linux and MacOS, the standard 'dfu-util' can be used. It can be installed via your package manager under Linux. On MacOS, it is available through MacPorts and Brew as a package.
- For ZuluSCSI V1.x boards, firmware can be flashed with the following command:
- `dfu-util --alt 0 --dfuse-address 0x08000000 --download ZuluSCSIv1_1_XXXXXX.bin`


DIP switches
------------
ZuluSCSI Blaster and RP2040 (Full Size) DIP switch settings are:
- INITIATOR: Enable SCSI initiator mode for imaging SCSI drives
- DEBUG LOG: Enable verbose debug log (saved to `zululog.txt`)
- TERMINATION: Enable SCSI termination
Later (Rev2023a) ZuluSCSI RP2040 and all ZuluSCSI Blaster Full Size boards have a bootloader button instead of a DIP switch. 

For ZuluSCSI V1.1, the DIP switch settings are as follows:

- DEBUG: Enable verbose debug log (saved to `zululog.txt`)
- TERM: Enable SCSI termination
- BOOT: Enable built-in USB bootloader, this DIP switch MUST remain off during normal operation.
- SW1: Enables/disables Macintosh/Apple specific mode-pages and device strings, which eases disk initialization when performing fresh installs on legacy Macintosh computers.

For ZuluSCSI V1.2, the DIP switch settings at SW301 are as follows:

- TERM: Enable SCSI termination
- DEBUG: Enable verbose debug logging via USB serial console.
- DIRECT/RAW: when in the factory-default OFF position, the entirety of the SD card is exposed as a single block device, and the device type is defined by the setting of the rotary DIP switch at SW404.
- QUIRKS: Enables/disables Macintosh/Apple specific mode-pages and device strings, which eases disk initialization when performing fresh installs on legacy Macintosh computers.

ZuluSCSI Mini has no DIP switches, so all optional configuration parameters must be defined in zuluscsi.ini

Physical eject button for CDROM
-------------------------------
CD-ROM drives can be configured to eject when a physical button is pressed.
If multiple image files are configured with `IMG0`..`IMG9` config settings, ejection will switch between them.
Two separate buttons are supported and they can eject different drives.

    [SCSI1]
    Type=2 # CDROM drive
    IMG0 = img0.iso
    IMG1 = ...
    EjectButton = 1

On ZuluSCSI V1.0 and V1.1 models, buttons are connected to J303 12-pin expansion header.
Button 1 is connected between `PE5` and `GND`, and button 2 is connected between `PE6` and `GND`.
Pin locations are also shown in [this image](docs/ZuluSCSI_v1_1_buttons.jpg).

On red ZuluSCSI Blaster and RP2040-based ZuluSCSI models, buttons are connected to the I2C pins.

Button 1 is connected between `SDA` and `GND` and button 2 is connected between `SCL` and `GND`.
On full-size models, the pins are available on expansion header J303 ([image](docs/ZuluSCSI_RP2040_buttons.jpg)).
On compact model, pins are available on 4-pin I2C header J305 ([image](docs/ZuluSCSI_RP2040_compact_buttons.jpg)).

SCSI initiator mode
-------------------
The **full-size** ZuluSCSI Blaster, ZuluSCSI RP2040, and Pico OSHW models support SCSI initiator mode for reading SCSI drives.
When enabled by the DIP switch, the ZuluSCSI Blaster/RP2040 will scan for SCSI drives on the bus and copy the data as `HDxx_imaged.hda` to the SD card.

LED indications in initiator mode:

- Short blink once a second: idle, searching for SCSI drives
- Fast blink 4 times per second: copying data. The blink acts as a progress bar: first it is short and becomes longer when data copying progresses.

The firmware retries reads up to 5 times and attempts to skip any sectors that have problems.
Any read errors are logged into `zululog.txt`.

Alternatively, if SD card is not inserted, the SCSI drive will be available to PC through USB connection.
The drive shows up as mass storage device (USB MSC).
Currently this primarily supports SCSI harddrives, but basic read access is supported for CD-ROM and other SCSI devices.

Depending on hardware setup, you may need to mount diode `D205` and jumper `JP201` to supply `TERMPWR` to the SCSI bus.
This is necessary if the drives do not supply their own SCSI terminator power.

ROM drive in microcontroller flash
----------------------------------
The new ZuluSCSI Blaster model supports storing up to 15.8 **megabytes** in flash, which can be used as a read-only bootable ROM drive.

All older ZuluSCSI RP2040 models support storing up to 1660kB image as a read-only drive in the flash chip on the PCB itself. This can be used as e.g. a boot floppy that is available even without SD card.

To initialize a ROM drive, name your image file as e.g. `HD0.rom`.
The drive type, SCSI ID and blocksize can be set in the filename the same way as for normal images.
On first boot, the LED will blink rapidly while the image is being loaded into flash memory.
Once loading is complete, the file is renamed to `HD0.rom_loaded` and the data is accessed from flash instead.

The status and maximum size of ROM drive are reported in `zululog.txt`.
To disable a previously programmed ROM drive, create empty file called `HD0.rom`.
If there is a `.bin` file with the same ID as the programmed ROM drive, it overrides the ROM drive.
There can be at most one ROM drive enabled at a time.

Kiosk mode for museums or demonstration setups
----------------------------------------------
The Kiosk mode is designed for vintage computer museums or other demonstration setups, where machines can be used by visitors but need to be easily restored to a pristine state.

At startup, all files with `.ori` extensions are copied to new volumes. For instance, `HD10_512.hda.ori` would be copied to `HD10_512.hda`.

Restoration takes a significant amount of time, so it should be used with small drives (it takes between 5 and 20 seconds to restore a 40MB hard drive depending on hardware). During the copy, the LED will blink with the pattern ON-OFF-ON-OFF-OFF. Each cycle corresponds to 5MB restored.

Rebooting the machine will not restore the files - you need to physically power-cycle the ZuluSCSI device.

Zulu Control Board
-------------------
The Zulu Control Board is a small device which plugs into the I2C connector of the ZuluSCSI Blaster (Currently, that is the only supported board)
It features an OLED screen, a rotary encoder, and two push buttons.
This allows you to both control and visualise various aspects of the ZuluSCSI.
Note: There are no error screens. Error must still be inspected in the log files.

On power up a splash screen will display the current firmware version:

![Splash](https://github.com/user-attachments/assets/64c03fdc-526e-461a-85c0-4f947cb282be)

Then, depending on whether the initiator switch is on or off, it will go into either normal mode or initiator mode.

Start Up
---------
There are various settings which can cause the ZuluSCSI to do tasks during startup. These are all long running and have dedicated progress screens.

If there is .rom image on the SD, then the image will be copied into ROM, this is what the progress screen will look like:

![ROM Copy](https://github.com/user-attachments/assets/aba70c61-24f7-4f89-a631-846f25ac0c7f)

If there is a special create file on the SD, then an image will be created, this is what the progress screen will look like:

![create](https://github.com/user-attachments/assets/d03cc8ca-aa89-4202-8680-a06d40a7b38e)

If there are images with the .ori extension for Kiosk mode, then they will be copied, this is what the progress screen will look like:

![kiosk](https://github.com/user-attachments/assets/d6c376d6-95dd-4f4b-9774-5c654451dd07)

All these screen share common elements (from top to bottom)
- Title showing the operation, for Kiosk mode it shows how many files are to be copied
- The progress bar with % complete
- The filename being copied
- The rate of copy and the elapsed time
- The remaining bytes and remaining time

Once these have finished, the normal boot sequence will resume

Normal Mode
-----------
Assuming the initiator switch is off, then the splash screen will display the mode:

![SplashNormal](https://github.com/user-attachments/assets/90b22cfe-5f58-4377-80ba-a0667d50badf)

and assuming caching is turned on, for a few seconds, the caching building screen will appear

![Build Cache](https://github.com/user-attachments/assets/494e6e3b-dc4d-4628-b167-fbb48e2b5e3f)

Once that is complete, the main screen will be displayed

Main Screen
----------

![Main](https://github.com/user-attachments/assets/6ce8386b-d671-43f4-95f7-f2fe6e81e5d0)

(TODO: update image)

This screen displays an overview of the SCSI devices.

Top righr will show an SD card icon. (if the card is removed, the icon will have a little cross in it)

Each item displays:
- The SCSI ID
- An empty circle represents nothing is loaded, a solid circle represents a loaded image
- If an image is loaded, the type is displayed as an icon. e.g. ID 0 is a CD, IDs 1-3 and 5 are Hard Drives, and ID 6 is a Magneto-optical Disk in the picture above
- If the image is stored in the ZuluSCSI's ROM, then chip icon is displayed (ID 6 in the picture above)

Turning the `rotary wheel` will select a loaded image (a little arrow will be visible next to the image, in the above picture it's on ID 0)

If a device is not browsable and the `eject` button is pressed trying to browse it, the following pop up will be displayed:

![WarningBrowseNotSupported](https://github.com/user-attachments/assets/14f0b488-361d-4831-b198-d0b588976bd0)


Info Screen
-----------
Clicking the `rotary button` will display info about the selected device:

![Info](https://github.com/user-attachments/assets/4d8be5b4-379b-4d89-b3a5-928d1a66fbd3)

The following is on the screen:
- The type of the image is displayed as an icon, and the SCSI ID is in the top right.
- The file name (it scrolls if it's too long for the display)
- the folder name (it scrolls if it's too long for the display)
- Whether it can be browsed
- The filesize of the image

Pressing the `User` button will return to the Main Screen

Browser Type Screen
-------------------
Depending on the way images are set up for browsing, some styles can have different browse modes.
If images are set up in config, either as:
- An ImgDir path is specified
- Use the standard folder naming of Device Type / SCSI ID. e.g.  CD0 for cd images for SCSI ID 0
  
Then the browsing type can be selected. To do this, long-press the `eject` button

Other image browsing modes, i.e. `IMGx` and naming file with the Device Type / SCSI ID prefix e.g. CD0 cannot access the browser type screen or use categories

![BrowserType](https://github.com/user-attachments/assets/c6b3a59b-1f2b-42bd-8a88-9944fe68d10e)

The first option is for the Folder-based Browser, which displays files and folders just like the structure on the SD card

The second option is for the Flat Browser, which flattens all files into a single list, removing the need to go into and out of folders (folder names are still displayed)

The remaining options are also Flat browser-based, but perform filtering based on the category (see category section)

Clicking either the `eject` or `rotary button` will go to the browser screen

Clicking `user` will return to the main menu

NOTE: The selected browse type is remembered for each SCSI ID. So, going to the browsing screen directly from the main menu will use the last selected browser type

Folder-based Browser
--------------------
![Browser](https://github.com/user-attachments/assets/ccb18e68-2d56-43b0-b8bf-5856cebf2f13)

The following is on the screen:
- The type of the image is displayed as an icon, and the SCSI ID is in the top right.
- The file/folder name (it scrolls if it's too long for the display)
- the folder name (it scrolls if it's too long for the display)
- The filesize of the image
- The type (File or Folder) and which item number and total it is in this folder

Turning the `rotary dial` with select files and folders, the last item in the list for nested folders is a folder called ".." which, if selected, goes back a folder

Clicking either the `eject` or `rotary button` on a folder will go to the browser screen

Clicking either the `eject`  on a file will load the image

Clicking `user` or long-pressing the `rotary button` in a nested folder will go back a directory, in the root folder, it will return to the main menu

During image loading, the following pop up will be displayed:

![Loading Image](https://github.com/user-attachments/assets/d2fd792e-b54c-49b3-a0ed-d327937597e5)

Flat Browser
------------
The flat browser is very similar to the Folder Browser, but without:
- any of the folders displayed as selectable items
- the .. folder

Turning the `rotary dial` with select files

Clicking  `eject` will load the image

Clicking `user` will return to the main menu

Categories
----------
Categories provide a way of filtering images, this can be useful for large image sets.
There can be up to 10 categories per SCSI ID and are specifc to that SCSI ID.

Assume that there are images for a sampler, and that they can be divided into various categories, eg. Drums, Synths, Vocals and Guitars.

To set the categories, the following config should be added to the SCSI device (assume this is for SCSI ID 0 and the images will be in a folder called ISO1)

    [SCSI0]
    Type=2 # CDROM drive
    ImgDir = "ISO1"
    Cat0 = "dDrums"
    Cat1 = "sSynths"
    Cat2 = "vVocals"
    Cat3 = "gGuitars"

The 4 categories are defined, and the first letter of the name is the code which represents that category (i.e `Cat0`, has a code of 'd' and a name of 'Drums')

Next, assume the following files are in the ISO1 folder:

    Sample Disk {v}.iso
    Drums 1 {d}.iso
    Drums 2 {d}.iso
    Licks {g}.iso
    Various Samples {dsvg}.iso

The letters in the {} brackets represent which categories each file belongs to. so:

    Sample Disk {v}.iso - is in the vocal category
    Drums 1 {d}.iso - is in the drums category
    Various Samples {dsvg}.iso - is in all 4 categories

So if you go to the Browser Type screen and select 'cat: drums', then the following 3 files would be browsable:

    Drums 1 {d}.iso
    Drums 2 {d}.iso
    Various Samples {dsvg}.iso

Controls Summary
--------------
Main Screen:
- turning the `rotary dial` will select a SCSI device
- click on `eject` - this loads the browser screen
- click on the `rotary button` - this goes to the info screen
- long click on `User` - this will display the splash screen (same as on boot)
- long click on `eject` - this loads the browser type screen

Info Screen:
- click on `user` - returns to main screen

Browse Type Screen:
- click on `user` - returns to main screen
- click on `eject` or `rotary button` - go to the selected Browser screen
  
Browser Screen (Folder Mode):
- turning the `rotary dial` will select a folder or file
- click on `user` in a nested folder will go back a directory, in the root folder it will return to the main menu
- click on `eject` or `rotary button` on the folder to navigate into that folder
- click on `eject` on a file to load the image 
  
Browser Screen (Flat  Mode):
- turning the `rotary dial` will select a file
- click on `user` - returns to main screen
- click on `eject` on a file to load the image 

No SD screen:
- This screen has no controls.

Initiator Mode
--------------
Assuming the initiator switch is on, then the splash screen will display the mode:

![InitiatorSplash](https://github.com/user-attachments/assets/146e65af-9647-4f2d-ab39-c448f4e05191)

and will the go to the scanning screen:

![scanning](https://github.com/user-attachments/assets/15f79ace-d881-479d-8023-f11c0efac276)

A circle with an X through it is the SCSI ID currently being scanned

Question marks are SCSI IDs which have no been checked yet

The empty circles are SCSI IDs which have been scanned

Once a drive is detected it will automatically go to the cloning screen:

![cloning](https://github.com/user-attachments/assets/b98c6b09-5f04-4126-a87b-37a148bdbbca)

Note: this is similar to the other copy screens witht he exception that the file name is not displayed, but the total number of retires and errors (skipped sector) is displayed.

Once the cloning is complete, it will return to the scanning screen and display the cloned drive:

![resultaftercloning](https://github.com/user-attachments/assets/58ae95b1-6ad6-458e-8d0c-d90e0a23204d)

NOTE: There are no controls in Initiator mode, it is a viewing-only mode

Control Board Config
--------------------
- `ControlBoardCache` - Enables caching of images. This will greatly improve performance when there are many images. set this to 1 to enable. Enabling will cause some wear on the SD card as the cache is built every time the card is inserted, and there is a slight delay on load (generally 1-2secs for large file sets). So if browsing is laggy, enable this

- `ControlBoardReverseRotary` - Some encoders work in the opposite direction, if you experience that, set this to 1

- `ControlBoardFlipDisplay` - Rotates the screen 180 degrees. set this to 1 to flip

- `ControlBoardDimDisplay` - if set to 1, then the screen will be a bit dimmer

- `ControlBoardScreenSaverTimeSec` - Sets the seconds before the screen savers starts. 0 means screen saver is disabled

- `ControlBoardShowCueFileName` - When disaplaying a bin/cue in a folder, the default is to show the folder name for the name. Setting this to 1 will display the cue file name

- `ControlBoardScreenSaverStyle` - There are several screen savers, each ones uses more resources, so if you see a performance drop try a lower numbered one:

- 0 - Random selection (each time the screen saver starts, a random one will be selected)
- 1 - Blank screen
- 2 - DVD Style Random ZuluSCSI logo
- 3 - DVD Style Bouncing ZuluSCSI logo
- 4 - Horizontal Scrolling Icons
- 5 - Vertical Raining Icons
- 6 - Light speed


Project structure
-----------------
- **src/ZuluSCSI.cpp**: Main portable SCSI implementation.
- **src/ZuluSCSI_disk.cpp**: Interface between SCSI2SD code and SD card reading.
- **src/ZuluSCSI_log.cpp**: Simple logging functionality, uses memory buffering.
- **src/ZuluSCSI_config.h**: Some compile-time options, usually no need to change.
- **lib/ZuluSCSI_platform_platformName**: Platform-specific code for each supported ZuluSCSI hardware platform.
- **lib/SCSI2SD**: SCSI2SD V6 code, used for SCSI command implementations.
- **lib/minIni**: Ini config file access library
- **lib/SdFat_NoArduino**: Modified version of [SdFat](https://github.com/greiman/SdFat) library for use without Arduino core.
- **utils/run_gdb.sh**: Helper script for debugging with st-link adapter. Displays SWO log directly in console.

To port the code to a new platform, see README in [lib/ZuluSCSI_platform_template](lib/ZuluSCSI_platform_template) folder.

Building
--------
This codebase uses [PlatformIO](https://platformio.org/).
To build run the command:

    pio run


Origins and License
-------------------

This firmware is derived from two sources, both under GPL 3 license:

* [SCSI2SD V6](http://www.codesrc.com/mediawiki/index.php/SCSI2SD)
* [BlueSCSI](https://github.com/erichelgeson/BlueSCSI), which in turn is derived from [ArdSCSIno-stm32](https://github.com/ztto/ArdSCSino-stm32).

Main program structure:

* SCSI command implementations are from SCSI2SD.
* SCSI physical layer code is mostly custom, with some inspiration from BlueSCSI.
* Image file access is derived from BlueSCSI.

Major changes from BlueSCSI and SCSI2SD include:

* Separation of platform-specific functionality to separate directory to ease porting.
* Originally ported to GD32F205 and then RP2040 (See commit [858620f](https://github.com/ZuluSCSI/ZuluSCSI-firmware/commit/858620f2855d29fbd5b3f905972523b4fe65fdea)).
* Removal of Arduino core dependency, as it was not currently available for GD32F205.
* Buffered log functions.
* Simultaneous transfer between SD card and SCSI for improved performance.
