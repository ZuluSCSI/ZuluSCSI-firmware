#if defined(CONTROL_BOARD) && !defined(ENABLE_AUDIO_OUTPUT_SPDIF)

#ifndef SYSTEMMODE_H
#define SYSTEMMODE_H

typedef enum
{
    SYSTEM_NORMAL,
	SYSTEM_INITIATOR
} SYSTEM_MODE;

#endif

#endif