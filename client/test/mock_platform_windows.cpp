// Windows side of the mock client: see mock_platform.h.
//
// This program is linked at Wow.exe's image base (0x00400000, fixed) with the
// client's build timestamp in its header, and it carries one large block of
// zeroed memory, so that its own image spans every client address the library
// reads, writes or patches. The library, loaded from the real DLL, cannot tell
// it from the game and patches it exactly as it would patch the game.

#include "mock_platform.h"

#include "../src/win.h"

DLLIMPORT void* WINAPI GetStdHandle(uint32_t which);
DLLIMPORT void WINAPI ExitProcess(uint32_t code);
DLLIMPORT void WINAPI Sleep(uint32_t milliseconds);
DLLIMPORT uint32_t WINAPI GetLastError();

namespace {

constexpr uint32_t STD_OUTPUT_HANDLE = static_cast<uint32_t>(-11);

// The linker places this just above the program's own few small sections,
// near 0x00404000. From there it has to reach past the highest client address
// the library uses (0x00D415BC); the test checks that it does.
constexpr size_t kArenaSize = 0x00958000;
uint8_t g_arena[kArenaSize];

void PrintNumber(uint32_t value) {
    char text[11];
    int length = 0;
    do {
        text[length++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    char reversed[12];
    for (int i = 0; i < length; ++i) {
        reversed[i] = text[length - 1 - i];
    }
    reversed[length] = '\0';
    platform::Print(reversed);
}

}  // namespace

namespace platform {

bool PrepareArena() {
    uint32_t oldProtect = 0;
    return VirtualProtect(g_arena, kArenaSize, PAGE_EXECUTE_READWRITE, &oldProtect) != 0;
}

uintptr_t ArenaBegin() {
    return reinterpret_cast<uintptr_t>(g_arena);
}

uintptr_t ArenaEnd() {
    return reinterpret_cast<uintptr_t>(g_arena) + kArenaSize;
}

bool LoadLibraryUnderTest() {
    if (LoadLibraryA("WotLK-ARPG.dll") != nullptr) {
        return true;
    }
    Print("LoadLibrary failed, Windows error ");
    PrintNumber(GetLastError());
    Print("\n");
    return false;
}

void Print(const char* text) {
    uint32_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    uint32_t written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, length, &written, nullptr);
}

void Sleep(uint32_t milliseconds) {
    ::Sleep(milliseconds);
}

void Exit(int code) {
    ExitProcess(static_cast<uint32_t>(code));
    for (;;) {
    }
}

}  // namespace platform

extern "C" void __cdecl Entry() {
    platform::Exit(MockMain());
}
