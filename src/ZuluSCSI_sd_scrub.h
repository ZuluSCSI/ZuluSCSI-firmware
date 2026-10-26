// Background scrubber for the SD card: releases erase units that are entirely
// zeros with the card's ERASE command, while the SCSI bus is quiet. See
// ZuluSCSI_sd_scrub.cpp for how it avoids getting in the way of the host.
//
// Off unless [SCSI] SdScrub = Yes is set in zuluscsi.ini; only used if the
// card reads erased blocks back as zeros (ZuluSCSI_sd_zero.h).

#pragma once

// Reads the settings, measures the card's erase behaviour once (a few seconds,
// the host is not served meanwhile) and starts the first pass. Call once the SD
// card and the images are set up and before the main loop runs.
void sdTrimStart();

// Stops the scrubber, e.g. when the card is changed.
void sdScrubReset();

// Call from the main loop; does nothing most of the time.
void sdScrubPoll();
