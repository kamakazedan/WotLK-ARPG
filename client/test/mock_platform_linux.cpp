// Linux side of the mock client: see mock_platform.h.
//
// A freestanding 32-bit program (no C library; system calls by hand) with the
// library's own sources compiled in. The handful of Windows functions the
// library calls are implemented here, and a block of memory is mapped at
// Wow.exe's image base, so the library's code runs unchanged: same 32-bit
// instructions, same calling conventions, same client addresses. The program
// itself is linked far away from that block (see run.sh).

#include "mock_platform.h"

#include "../src/win.h"  // built with DLLIMPORT=extern "C": declares what is defined below

extern "C" int WINAPI DllMain(void* module, uint32_t reason, void* reserved);

namespace {

constexpr uintptr_t kArenaBase = 0x00400000;
constexpr size_t kArenaSize = 0x009FD000;  // the size of the real image
constexpr uint32_t kBuildTimestamp = 0x4C2452FE;

// i386 Linux system call numbers.
constexpr int kSysWrite = 4;
constexpr int kSysOpen = 5;
constexpr int kSysMmap = 90;  // the original form: one pointer to six arguments
constexpr int kSysNanosleep = 162;
constexpr int kSysExitGroup = 252;
constexpr int kSysClockGettime = 265;

int Syscall(int number, intptr_t a = 0, intptr_t b = 0, intptr_t c = 0) {
    int result;
    __asm__ volatile("int $0x80" : "=a"(result) : "a"(number), "b"(a), "c"(b), "d"(c) : "memory");
    return result;
}

struct Timespec {
    int32_t seconds;
    int32_t nanoseconds;
};

void* Map(uintptr_t address, size_t size, bool fixed) {
    constexpr uint32_t kReadWriteExecute = 7;
    constexpr uint32_t kPrivateAnonymous = 0x22;
    constexpr uint32_t kFixed = 0x10;
    uint32_t arguments[6] = {static_cast<uint32_t>(address), static_cast<uint32_t>(size), kReadWriteExecute,
                             kPrivateAnonymous | (fixed ? kFixed : 0), 0xFFFFFFFFu, 0};
    const uint32_t result = static_cast<uint32_t>(Syscall(kSysMmap, reinterpret_cast<intptr_t>(arguments)));
    return result >= 0xFFFFF000u ? nullptr : reinterpret_cast<void*>(static_cast<uintptr_t>(result));
}

}  // namespace

// ---------------------------------------------------------------------------
// The Windows functions the library uses
// ---------------------------------------------------------------------------

extern "C" int WINAPI VirtualProtect(void*, size_t, uint32_t, uint32_t* oldProtect) {
    *oldProtect = PAGE_EXECUTE_READWRITE;  // everything here already is
    return 1;
}

extern "C" void* WINAPI VirtualAlloc(void*, size_t size, uint32_t, uint32_t) {
    return Map(0, size, false);
}

extern "C" int WINAPI FlushInstructionCache(void*, const void*, size_t) {
    return 1;
}

extern "C" void* WINAPI GetCurrentProcess() {
    return reinterpret_cast<void*>(static_cast<intptr_t>(-1));
}

extern "C" void* WINAPI LoadLibraryA(const char*) {
    return nullptr;  // no other library to chain to
}

extern "C" void* WINAPI GetModuleHandleA(const char*) {
    return reinterpret_cast<void*>(kArenaBase);
}

// "./x": the library writes its log next to the program, so into the
// directory the test is run from.
extern "C" uint32_t WINAPI GetModuleFileNameA(void*, char* buffer, uint32_t size) {
    const char path[] = "./x";
    uint32_t length = 0;
    while (path[length] != '\0' && length + 1 < size) {
        buffer[length] = path[length];
        ++length;
    }
    buffer[length] = '\0';
    return length;
}

extern "C" void* WINAPI CreateFileA(const char* name, uint32_t, uint32_t, void*, uint32_t, uint32_t, void*) {
    constexpr int kWriteCreateTruncate = 01101;
    const int file = Syscall(kSysOpen, reinterpret_cast<intptr_t>(name), kWriteCreateTruncate, 0644);
    return file < 0 ? INVALID_HANDLE_VALUE : reinterpret_cast<void*>(static_cast<intptr_t>(file));
}

extern "C" int WINAPI WriteFile(void* file, const void* buffer, uint32_t size, uint32_t* written, void*) {
    const int count = Syscall(kSysWrite, reinterpret_cast<intptr_t>(file), reinterpret_cast<intptr_t>(buffer),
                              static_cast<intptr_t>(size));
    if (written != nullptr) {
        *written = count > 0 ? static_cast<uint32_t>(count) : 0;
    }
    return count >= 0 ? 1 : 0;
}

extern "C" uint32_t WINAPI GetTickCount() {
    constexpr int kMonotonic = 1;
    Timespec now = {0, 0};
    Syscall(kSysClockGettime, kMonotonic, reinterpret_cast<intptr_t>(&now));
    return static_cast<uint32_t>(now.seconds) * 1000u + static_cast<uint32_t>(now.nanoseconds) / 1000000u;
}

extern "C" int WINAPI DisableThreadLibraryCalls(void*) {
    return 1;
}

// ---------------------------------------------------------------------------
// The platform interface
// ---------------------------------------------------------------------------

namespace platform {

bool PrepareArena() {
    if (Map(kArenaBase, kArenaSize, true) != reinterpret_cast<void*>(kArenaBase)) {
        return false;
    }
    // The two header fields the library reads to recognise the client.
    constexpr uint32_t kHeaderOffset = 0x80;
    *reinterpret_cast<uint16_t*>(kArenaBase) = 0x5A4D;                    // "MZ"
    *reinterpret_cast<uint32_t*>(kArenaBase + 0x3C) = kHeaderOffset;
    *reinterpret_cast<uint32_t*>(kArenaBase + kHeaderOffset) = 0x4550;   // "PE"
    *reinterpret_cast<uint32_t*>(kArenaBase + kHeaderOffset + 8) = kBuildTimestamp;
    return true;
}

uintptr_t ArenaBegin() {
    return kArenaBase + 0x1000;  // the first page is the header
}

uintptr_t ArenaEnd() {
    return kArenaBase + kArenaSize;
}

bool LoadLibraryUnderTest() {
    return DllMain(nullptr, DLL_PROCESS_ATTACH, nullptr) != 0;
}

void Print(const char* text) {
    size_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    Syscall(kSysWrite, 1, reinterpret_cast<intptr_t>(text), static_cast<intptr_t>(length));
}

void Sleep(uint32_t milliseconds) {
    Timespec request = {static_cast<int32_t>(milliseconds / 1000),
                        static_cast<int32_t>(milliseconds % 1000) * 1000000};
    Syscall(kSysNanosleep, reinterpret_cast<intptr_t>(&request), 0);
}

void Exit(int code) {
    Syscall(kSysExitGroup, code);
    for (;;) {
    }
}

}  // namespace platform

extern "C" void _start() {
    platform::Exit(MockMain());
}
