// Guarded code patching for a known executable.
//
// Every patch names the bytes it expects to find. If the bytes differ, nothing
// is written and the caller turns the feature off: an unexpected executable or
// another mod's patch must never lead to a crash.
//
// The one foreign patch that is understood is another library's 5-byte jump
// (E9 rel32) at the same spot, which is how detour libraries hook a function,
// or, at a call instruction, another library's call (E8 rel32). In that case
// this hook is placed in front of theirs and theirs still runs.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace hook {

enum class Result {
    kInstalled,  // placed over the stock bytes
    kChained,    // placed in front of another library's jump
    kMismatch,   // unexpected bytes: nothing written
    kFailed,     // could not allocate or change memory protection: nothing written
};

struct Site {
    const char* name;
    uintptr_t address;
    const uint8_t* expected;  // stock bytes at the address
    uint8_t expectedLength;
    uint8_t stolenLength;     // whole, position-independent instructions covering at least 5 bytes
};

// Redirects the function at site.address to `replacement`. `*original` then
// holds an address that behaves like the function did before this call.
Result Detour(const Site& site, void* replacement, void** original);

// For a function ending in `ret` followed by padding: runs `after()` just
// before it returns. site.expected must be the ret and at least 4 padding bytes.
Result CallBeforeReturn(const Site& site, void (*after)());

// For a `call rel32` instruction inside a function: makes it call
// `replacement` instead. `*original` then holds the address it called before,
// to be called with the same arguments and calling convention. site.expected
// must be the five bytes of the stock instruction; site.stolenLength is not
// used.
Result RedirectCall(const Site& site, void* replacement, void** original);

bool IsActive(Result result);
const char* Describe(Result result);

}  // namespace hook
