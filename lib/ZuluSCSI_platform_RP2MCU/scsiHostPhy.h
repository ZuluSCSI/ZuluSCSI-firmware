/** 
 * ZuluSCSI™ - Copyright (c) 2022-2025 Rabbit Hole Computing™
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

// Host side SCSI physical interface.
// Used in initiator to interface to an SCSI drive.

#pragma once

#include <stdint.h>
#include <stdbool.h>

// Request to stop activity and reset the bus
extern volatile int g_scsiHostPhyReset;
extern uint8_t g_scsiHostPhySelectionStage;
extern uint32_t g_scsiHostPhySelectionSignals;

// Release bus and pulse RST signal, initialize PHY to host mode.
void scsiHostPhyReset(void);
void scsiHostPhyActivateNoReset(void);
// Return the initiator-only ACK/ATN pins to inputs before target-mode restore.
void scsiHostPhyDeactivate(void);

// Wait until both BSY and SEL have remained inactive for stable_us.
// This is required when changing from target mode to initiator mode on a
// live multi-initiator bus.

// Select a device, id 0-7.
// target_id - target device id 0-7
// initiator_id - host device id 0-7
// Returns true if the target answers to selection request.
bool scsiHostPhySelect(int target_id, uint8_t initiator_id, bool request_atn);

// Raw active-high signal snapshot for diagnostics. Bits 0..6 are
// BSY, SEL, REQ, CD, IO, MSG and ATN; bits 8..15 contain the data bus.
uint32_t scsiHostPhyGetSignals(void);

// Compact Blaster GPIO snapshot for diagnostics. Bits 0..8 are pin
// directions, 9..17 are pad inputs, and 18..26 are output-latch levels for
// REQ, CD, MSG, IO, ATN, ACK, SEL, BSY and RST respectively.
uint32_t scsiHostPhyGetGPIOState(void);

// Set SCSI ATN signal to request MESSAGE_OUT phase
void scsiHostPhySetATN(bool atn);

// Set wide (16-bit) bus mode
// busWidth - 0 for 8-bit, 1 for 16-bit
void scsiHostSetBusWidth(int busWidth);

// Read the current communication phase as signaled by the target
// Matches SCSI_PHASE enumeration from scsi.h.
int scsiHostPhyGetPhase();

// Returns true if the device has asserted REQ signal, i.e. data waiting
bool scsiHostRequestWaiting();

// Blocking data transfer
// These return the actual number of bytes transferred.
uint32_t scsiHostWrite(const uint8_t *data, uint32_t count);
uint32_t scsiHostRead(uint8_t *data, uint32_t count);

// Release bus signals and expect the target to do the same.
// Cycles ACK in case target still holds BSY and REQ.
void scsiHostWaitBusFree();

// Release all bus signals
void scsiHostPhyRelease();
