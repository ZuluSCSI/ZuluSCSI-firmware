//	Copyright (C) 2013 Michael McMaster <michael@codesrc.com>
//
//	This file is part of SCSI2SD.
//
//	SCSI2SD is free software: you can redistribute it and/or modify
//	it under the terms of the GNU General Public License as published by
//	the Free Software Foundation, either version 3 of the License, or
//	(at your option) any later version.
//
//	SCSI2SD is distributed in the hope that it will be useful,
//	but WITHOUT ANY WARRANTY; without even the implied warranty of
//	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//	GNU General Public License for more details.
//
//	You should have received a copy of the GNU General Public License
//	along with SCSI2SD.  If not, see <http://www.gnu.org/licenses/>.

#include "scsi.h"
#include "diagnostic.h"

#include <string.h>

static const uint8_t SupportedDiagnosticPages[] =
{
0x00, // Page Code
0x00, // Reserved
0x02, // Page length
0x00, // Support "Supported diagnostic page"
0x40  // Support "Translate address page"
};

void scsiSendDiagnostic()
{
	// SEND DIAGNOSTIC
	// Pretend to do self-test. Actual data is returned via the
	// RECEIVE DIAGNOSTIC RESULTS command.
	int selfTest = scsiDev.cdb[1] & 0x04;
	uint32_t paramLength =
		(((uint32_t) scsiDev.cdb[3]) << 8) +
		scsiDev.cdb[4];

	if (!selfTest)
	{
		// Initiator sends us page data.
		scsiDev.dataLen = paramLength;
		scsiDev.phase = DATA_OUT;

		if (scsiDev.dataLen > sizeof (scsiDev.data))
		{
			// Nowhere to store this data!
			// Shouldn't happen - our buffer should be many magnitudes larger
			// than the required size for diagnostic parameters.
			scsiDev.target->sense.code = ILLEGAL_REQUEST;
			scsiDev.target->sense.asc = INVALID_FIELD_IN_CDB;
			scsiDev.status = CHECK_CONDITION;
			scsiDev.phase = STATUS;
		}
	}
	else
	{
		// Default command result will be a status of GOOD anyway.
	}
}

void scsiReceiveDiagnostic()
{
	// RECEIVE DIAGNOSTIC RESULTS
	// We assume scsiDev.data contains the contents of a previous
	// SEND DIAGNOSTICS command.  We only care about the page-code part
	// of the parameter list.
	uint8_t pageCode = scsiDev.data[0];

	int allocLength =
		(((uint16_t) scsiDev.cdb[3]) << 8) +
		scsiDev.cdb[4];


	if (pageCode == 0x00)
	{
		memcpy(
			scsiDev.data,
			SupportedDiagnosticPages,
			sizeof(SupportedDiagnosticPages));
		scsiDev.dataLen = sizeof(SupportedDiagnosticPages);
		scsiDev.phase = DATA_IN;
	}
	else if (pageCode == 0x40)
	{
		// Translate between logical block address, physical sector address, or
		// physical bytes.
		uint8_t suppliedFmt = scsiDev.data[4] & 0x7;
		uint8_t translateFmt = scsiDev.data[5] & 0x7;

		// Convert each supplied address back to a simple
		// 64bit linear address, then convert back again.
		uint64_t fromByteAddr =
			scsiByteAddress(
				scsiDev.target->liveCfg.bytesPerSector,
				scsiDev.target->cfg->headsPerCylinder,
				scsiDev.target->cfg->sectorsPerTrack,
				suppliedFmt,
				&scsiDev.data[6]);

		scsiSaveByteAddress(
			scsiDev.target->liveCfg.bytesPerSector,
			scsiDev.target->cfg->headsPerCylinder,
			scsiDev.target->cfg->sectorsPerTrack,
			translateFmt,
			fromByteAddr,
			&scsiDev.data[6]);

		// Fill out the rest of the response.
		// (Clear out any optional bits).
		scsiDev.data[4] = suppliedFmt;
		scsiDev.data[5] = translateFmt;

		scsiDev.dataLen = 14;
		scsiDev.phase = DATA_IN;
	}
	else
	{
		// error.
		scsiDev.status = CHECK_CONDITION;
		scsiDev.target->sense.code = ILLEGAL_REQUEST;
		scsiDev.target->sense.asc = INVALID_FIELD_IN_CDB;
		scsiDev.phase = STATUS;
	}

	if (scsiDev.phase == DATA_IN && scsiDev.dataLen > allocLength)
	{
		// simply truncate the response.
		scsiDev.dataLen = allocLength;
	}

	{
		// Set the first byte to indicate LUN presence.
		if (scsiDev.lun) // We only support lun 0
		{
			scsiDev.data[0] = 0x7F;
		}
	}
}

void scsiReadBuffer()
{
	// READ BUFFER
	// Used for testing the speed of the SCSI interface.
	uint8_t mode = scsiDev.data[1] & 7;

	int allocLength =
		(((uint32_t) scsiDev.cdb[6]) << 16) +
		(((uint32_t) scsiDev.cdb[7]) << 8) +
		scsiDev.cdb[8];

	if (mode == 0)
	{
		uint32_t maxSize = sizeof(scsiDev.data) - 4;
		// 4 byte header
		scsiDev.data[0] = 0;
		scsiDev.data[1] = (maxSize >> 16) & 0xff;
		scsiDev.data[2] = (maxSize >> 8) & 0xff;
		scsiDev.data[3] = maxSize & 0xff;

		scsiDev.dataLen =
			(allocLength > sizeof(scsiDev.data)) ? sizeof(scsiDev.data) : allocLength;
		scsiDev.phase = DATA_IN;
	}
	else if (mode == 0x2 && (scsiDev.cdb[2] == 0))
	{
		// TODO support BUFFER OFFSET fields in CDB
		scsiDev.dataLen =
			(allocLength > sizeof(scsiDev.data)) ? sizeof(scsiDev.data) : allocLength;
		scsiDev.phase = DATA_IN;
	}
	else if (mode == 0x3)
	{
		uint32_t maxSize = sizeof(scsiDev.data) - 4;
		// 4 byte header
		scsiDev.data[0] = 0;
		scsiDev.data[1] = (maxSize >> 16) & 0xff;
		scsiDev.data[2] = (maxSize >> 8) & 0xff;
		scsiDev.data[3] = maxSize & 0xff;

		scsiDev.dataLen =
			(allocLength > 4) ? 4: allocLength;
		scsiDev.phase = DATA_IN;
	}
	else
	{
		// error.
		scsiDev.status = CHECK_CONDITION;
		scsiDev.target->sense.code = ILLEGAL_REQUEST;
		scsiDev.target->sense.asc = INVALID_FIELD_IN_CDB;
		scsiDev.phase = STATUS;
	}
}

// Callback after the DATA OUT phase is complete.
void doWriteBuffer(void)
{
	if (scsiDev.status == GOOD) // skip if we've already encountered an error
	{
		// scsiDev.dataLen bytes are in scsiDev.data
		// Don't shift it down 4 bytes ... this space is taken by
		// the read buffer header anyway
		scsiDev.phase = STATUS;
	}
}

void scsiWriteBuffer()
{
	// WRITE BUFFER
	// Modes 0 (combined header and data) and 2 (data) are used for
	// testing the speed of the SCSI interface. Modes 4/5 (download
	// microcode / download microcode and save) are accepted too --
	// confirmed on real AS/400 hardware (9401-P02 CISC) that OS/400's
	// "reload Licensed Internal Code" IPL step sends WRITE BUFFER mode
	// 5. Zulu has no real on-disk microcode to update, so the payload
	// is simply accepted and discarded, matching doWriteBuffer() below,
	// which never persists received data anywhere regardless of mode.
	//
	// The mode field lives in the CDB (byte 1, bits 2:0), not in
	// scsiDev.data -- that's the DATA_OUT payload buffer, still holding
	// whatever the previous command left there at this point, since the
	// DATA_OUT phase for *this* command hasn't happened yet. Reading it
	// from scsiDev.data[1] here was effectively reading uninitialized/
	// stale garbage instead of the actual requested mode: confirmed via
	// a real-hardware log where the identical WRITE BUFFER mode-5 CDB
	// was rejected with ILLEGAL_REQUEST/INVALID_FIELD_IN_CDB the first
	// time and then succeeded on an immediate retry, once an
	// intervening REQUEST SENSE had overwritten scsiDev.data with
	// different leftover content.
	uint8_t mode = scsiDev.cdb[1] & 7;

	int allocLength =
		(((uint32_t) scsiDev.cdb[6]) << 16) +
		(((uint32_t) scsiDev.cdb[7]) << 8) +
		scsiDev.cdb[8];

	if ((mode == 0 || mode == 2 || mode == 4 || mode == 5) && allocLength <= sizeof(scsiDev.data))
	{
		scsiDev.dataLen = allocLength;
		scsiDev.phase = DATA_OUT;
		scsiDev.postDataOutHook = doWriteBuffer;
	}
	else
	{
		// error.
		scsiDev.status = CHECK_CONDITION;
		scsiDev.target->sense.code = ILLEGAL_REQUEST;
		scsiDev.target->sense.asc = INVALID_FIELD_IN_CDB;
		scsiDev.phase = STATUS;
	}
}


