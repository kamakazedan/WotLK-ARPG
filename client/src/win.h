// The handful of Win32 declarations the library needs.
//
// The library is built freestanding (no Windows SDK, no C runtime), so that
// the same clang + lld toolchain builds it on any OS and the resulting DLL
// depends on nothing but kernel32.dll.
//
// Two hooks for the tests in test/, which run this code outside Windows:
//   - a classic include guard (not #pragma once), so a test can define it up
//     front and supply its own stand-ins for everything in here;
//   - DLLIMPORT can be predefined, so a test can link these names against its
//     own implementations instead of importing them from a DLL.
#ifndef ARPG_WIN_H
#define ARPG_WIN_H

#include <stddef.h>
#include <stdint.h>

#define WINAPI __stdcall
#ifndef DLLIMPORT
#define DLLIMPORT extern "C" __declspec(dllimport)
#endif

constexpr uint32_t PAGE_EXECUTE_READWRITE = 0x40;
constexpr uint32_t MEM_COMMIT = 0x1000;
constexpr uint32_t MEM_RESERVE = 0x2000;
constexpr uint32_t GENERIC_WRITE = 0x40000000;
constexpr uint32_t FILE_SHARE_READ = 0x1;
constexpr uint32_t CREATE_ALWAYS = 2;
constexpr uint32_t FILE_ATTRIBUTE_NORMAL = 0x80;
constexpr uint32_t DLL_PROCESS_ATTACH = 1;
#define INVALID_HANDLE_VALUE (reinterpret_cast<void*>(static_cast<intptr_t>(-1)))

DLLIMPORT int WINAPI VirtualProtect(void* address, size_t size, uint32_t protect, uint32_t* oldProtect);
DLLIMPORT void* WINAPI VirtualAlloc(void* address, size_t size, uint32_t type, uint32_t protect);
DLLIMPORT int WINAPI FlushInstructionCache(void* process, const void* address, size_t size);
DLLIMPORT void* WINAPI GetCurrentProcess();
DLLIMPORT void* WINAPI LoadLibraryA(const char* name);
DLLIMPORT void* WINAPI GetModuleHandleA(const char* name);
DLLIMPORT uint32_t WINAPI GetModuleFileNameA(void* module, char* buffer, uint32_t size);
DLLIMPORT void* WINAPI CreateFileA(const char* name, uint32_t access, uint32_t share, void* security,
                                   uint32_t disposition, uint32_t flags, void* templateFile);
DLLIMPORT int WINAPI WriteFile(void* file, const void* buffer, uint32_t size, uint32_t* written, void* overlapped);
DLLIMPORT uint32_t WINAPI GetTickCount();
DLLIMPORT int WINAPI DisableThreadLibraryCalls(void* module);

#endif  // ARPG_WIN_H
