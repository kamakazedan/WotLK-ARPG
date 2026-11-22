// What the mock client (mock_client.cpp) needs from the system it runs on.
//
// There are two implementations:
//   mock_platform_windows.cpp  a real 32-bit Windows program that loads the
//                              real WotLK-ARPG.dll, the way the game does;
//   mock_platform_linux.cpp    a 32-bit Linux program with the library's
//                              sources compiled in, for a quick run on the
//                              machine that builds it.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace platform {

// The memory standing in for Wow.exe's image. Once prepared it is readable,
// writable and executable, starts at or below the lowest client address the
// library uses and ends above the highest, and the image base holds a header
// with the build's timestamp.
bool PrepareArena();
uintptr_t ArenaBegin();
uintptr_t ArenaEnd();

// Brings the library under test to life: its start-up code has run, in this
// process, when this returns true.
bool LoadLibraryUnderTest();

void Print(const char* text);
void Sleep(uint32_t milliseconds);
[[noreturn]] void Exit(int code);

}  // namespace platform

// The test itself; the platform's entry point calls it and exits with the result.
int MockMain();
