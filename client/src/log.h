// A plain text log next to Wow.exe (WotLK-ARPG.log), rewritten on every start.
#pragma once

#include <stdint.h>

namespace logfile {

void Open();

// Minimal printf: %s string, %d int, %u unsigned, %x 32-bit hex (8 digits),
// %B byte dump (const uint8_t* then int count). One line per call.
void Write(const char* format, ...);

}  // namespace logfile
