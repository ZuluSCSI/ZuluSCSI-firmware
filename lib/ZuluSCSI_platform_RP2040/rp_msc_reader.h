/* TODO - Header. By ZZJ. */

#ifndef RP_MSC_READER_H
#define RP_MSC_READER_H

#ifdef PLATFORM_CARDREADER

// wait up to this long during init sequence for USB enumeration to enter card reader
#define CR_ENUM_TIMEOUT 1000

// true if  we should enter cardreader mode
bool shouldEnterReader();

// run cardreader mode. does not return currently
void runCardReader();

#endif
#endif