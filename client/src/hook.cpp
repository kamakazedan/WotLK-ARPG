#include "hook.h"

#include "log.h"
#include "win.h"

namespace hook {
namespace {

constexpr uint8_t kJump = 0xE9;  // jmp rel32
constexpr uint8_t kCall = 0xE8;  // call rel32
constexpr uint8_t kRet = 0xC3;
constexpr uint8_t kNop = 0x90;
constexpr size_t kJumpLength = 5;
constexpr size_t kMaxPatch = 16;

// Executable scratch memory for trampolines. Never freed: hooks stay for the
// life of the process.
constexpr size_t kPoolSize = 4096;
uint8_t* g_pool = nullptr;
size_t g_poolUsed = 0;

uint8_t* Allocate(size_t size) {
    if (g_pool == nullptr) {
        g_pool = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, kPoolSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (g_pool == nullptr) {
            return nullptr;
        }
    }
    size = (size + 15) & ~static_cast<size_t>(15);
    if (g_poolUsed + size > kPoolSize) {
        return nullptr;
    }
    uint8_t* block = g_pool + g_poolUsed;
    g_poolUsed += size;
    return block;
}

bool Equal(const uint8_t* a, const uint8_t* b, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

// Writes `opcode rel32` into `out`, encoded as if it sat at address `at`.
void EmitRelative(uint8_t* out, uint8_t opcode, uintptr_t at, uintptr_t target) {
    const uint32_t relative = static_cast<uint32_t>(target - (at + kJumpLength));
    out[0] = opcode;
    out[1] = static_cast<uint8_t>(relative);
    out[2] = static_cast<uint8_t>(relative >> 8);
    out[3] = static_cast<uint8_t>(relative >> 16);
    out[4] = static_cast<uint8_t>(relative >> 24);
}

// Where the `jmp rel32` or `call rel32` at `jump` goes.
uintptr_t JumpTarget(const uint8_t* jump) {
    const uint32_t bits = static_cast<uint32_t>(jump[1]) | static_cast<uint32_t>(jump[2]) << 8 |
        static_cast<uint32_t>(jump[3]) << 16 | static_cast<uint32_t>(jump[4]) << 24;
    const intptr_t relative = static_cast<int32_t>(bits);  // signed: jumps go both ways
    return reinterpret_cast<uintptr_t>(jump) + kJumpLength + static_cast<uintptr_t>(relative);
}

bool WriteCode(uintptr_t address, const uint8_t* bytes, size_t count) {
    void* target = reinterpret_cast<void*>(address);
    uint32_t oldProtect = 0;
    if (VirtualProtect(target, count, PAGE_EXECUTE_READWRITE, &oldProtect) == 0) {
        return false;
    }
    uint8_t* out = static_cast<uint8_t*>(target);
    for (size_t i = 0; i < count; ++i) {
        out[i] = bytes[i];
    }
    uint32_t ignored = 0;
    VirtualProtect(target, count, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, count);
    return true;
}

Result Mismatch(const Site& site) {
    logfile::Write("  %s at %x: unexpected bytes [%B], expected [%B]. Left alone.", site.name,
                   static_cast<uint32_t>(site.address), reinterpret_cast<const uint8_t*>(site.address),
                   static_cast<int>(site.expectedLength), site.expected, static_cast<int>(site.expectedLength));
    return Result::kMismatch;
}

Result Failed(const Site& site, const char* why) {
    logfile::Write("  %s at %x: %s. Left alone.", site.name, static_cast<uint32_t>(site.address), why);
    return Result::kFailed;
}

}  // namespace

Result Detour(const Site& site, void* replacement, void** original) {
    const uint8_t* code = reinterpret_cast<const uint8_t*>(site.address);
    if (site.stolenLength < kJumpLength || site.stolenLength > kMaxPatch) {
        return Failed(site, "bad site definition");
    }

    uint8_t* trampoline = nullptr;
    size_t patchLength = 0;
    Result result = Result::kInstalled;

    if (Equal(code, site.expected, site.expectedLength)) {
        // Stock code: the trampoline runs the instructions about to be
        // overwritten, then continues in the original function.
        trampoline = Allocate(site.stolenLength + kJumpLength);
        if (trampoline == nullptr) {
            return Failed(site, "out of trampoline memory");
        }
        for (size_t i = 0; i < site.stolenLength; ++i) {
            trampoline[i] = code[i];
        }
        uint8_t* back = trampoline + site.stolenLength;
        EmitRelative(back, kJump, reinterpret_cast<uintptr_t>(back), site.address + site.stolenLength);
        patchLength = site.stolenLength;
    } else if (code[0] == kJump) {
        // Another library got here first. Calling "the original" means
        // jumping to wherever its jump went, so its hook keeps working.
        trampoline = Allocate(kJumpLength);
        if (trampoline == nullptr) {
            return Failed(site, "out of trampoline memory");
        }
        EmitRelative(trampoline, kJump, reinterpret_cast<uintptr_t>(trampoline), JumpTarget(code));
        patchLength = kJumpLength;
        result = Result::kChained;
    } else {
        return Mismatch(site);
    }

    uint8_t patch[kMaxPatch];
    EmitRelative(patch, kJump, site.address, reinterpret_cast<uintptr_t>(replacement));
    for (size_t i = kJumpLength; i < patchLength; ++i) {
        patch[i] = kNop;
    }

    *original = trampoline;  // valid before the patch goes live
    if (!WriteCode(site.address, patch, patchLength)) {
        *original = nullptr;
        return Failed(site, "could not make the code writable");
    }
    logfile::Write("  %s at %x: %s.", site.name, static_cast<uint32_t>(site.address), Describe(result));
    return result;
}

Result CallBeforeReturn(const Site& site, void (*after)()) {
    const uint8_t* code = reinterpret_cast<const uint8_t*>(site.address);
    if (site.expectedLength < kJumpLength || site.expected[0] != kRet) {
        return Failed(site, "bad site definition");
    }

    Result result = Result::kInstalled;
    uintptr_t previous = 0;
    if (Equal(code, site.expected, site.expectedLength)) {
        // Stock code: a ret with padding behind it.
    } else if (code[0] == kJump) {
        previous = JumpTarget(code);
        result = Result::kChained;
    } else {
        return Mismatch(site);
    }

    // thunk: save the registers a called function may overwrite (one of them
    // holds the hooked function's return value), call after, restore them,
    // then do what was there before: ret, or the other library's jump.
    static const uint8_t kSave[] = {0x50, 0x51, 0x52};     // push eax; push ecx; push edx
    static const uint8_t kRestore[] = {0x5A, 0x59, 0x58};  // pop edx; pop ecx; pop eax
    uint8_t* thunk = Allocate(sizeof(kSave) + kJumpLength + sizeof(kRestore) + kJumpLength);
    if (thunk == nullptr) {
        return Failed(site, "out of trampoline memory");
    }
    uint8_t* out = thunk;
    for (uint8_t byte : kSave) {
        *out++ = byte;
    }
    EmitRelative(out, kCall, reinterpret_cast<uintptr_t>(out), reinterpret_cast<uintptr_t>(after));
    out += kJumpLength;
    for (uint8_t byte : kRestore) {
        *out++ = byte;
    }
    uint8_t* tail = out;
    if (previous != 0) {
        EmitRelative(tail, kJump, reinterpret_cast<uintptr_t>(tail), previous);
    } else {
        tail[0] = kRet;
    }

    uint8_t patch[kJumpLength];
    EmitRelative(patch, kJump, site.address, reinterpret_cast<uintptr_t>(thunk));
    if (!WriteCode(site.address, patch, kJumpLength)) {
        return Failed(site, "could not make the code writable");
    }
    logfile::Write("  %s at %x: %s.", site.name, static_cast<uint32_t>(site.address), Describe(result));
    return result;
}

Result RedirectCall(const Site& site, void* replacement, void** original) {
    const uint8_t* code = reinterpret_cast<const uint8_t*>(site.address);
    if (site.expectedLength != kJumpLength || site.expected[0] != kCall) {
        return Failed(site, "bad site definition");
    }

    Result result = Result::kInstalled;
    if (Equal(code, site.expected, site.expectedLength)) {
        // Stock code.
    } else if (code[0] == kCall) {
        // Another library already sends this call somewhere else. Calling
        // "the original" then means calling its routine.
        result = Result::kChained;
    } else {
        return Mismatch(site);
    }

    uint8_t patch[kJumpLength];
    EmitRelative(patch, kCall, site.address, reinterpret_cast<uintptr_t>(replacement));
    *original = reinterpret_cast<void*>(JumpTarget(code));  // valid before the patch goes live
    if (!WriteCode(site.address, patch, kJumpLength)) {
        *original = nullptr;
        return Failed(site, "could not make the code writable");
    }
    logfile::Write("  %s at %x: %s.", site.name, static_cast<uint32_t>(site.address), Describe(result));
    return result;
}

bool IsActive(Result result) {
    return result == Result::kInstalled || result == Result::kChained;
}

const char* Describe(Result result) {
    switch (result) {
        case Result::kInstalled:
            return "installed";
        case Result::kChained:
            return "installed in front of another library's hook";
        case Result::kMismatch:
            return "unexpected bytes";
        case Result::kFailed:
            return "failed";
    }
    return "?";
}

}  // namespace hook
