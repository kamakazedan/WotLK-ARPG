// Host-side test of the hook engine (src/hook.cpp).
//
// The library itself only runs inside the game, but the code that rewrites
// machine code does not care which program it is in. This test compiles the
// real hook.cpp for Linux x86-64, points it at small hand-assembled functions
// and then calls them, covering: a hook over stock bytes, a refusal on
// unexpected bytes, a hook in front of another library's hook, and the
// call-before-return patch in both its plain and chained form.
//
// Build and run with test/run.sh. Linux x86-64 only.

#define ARPG_WIN_H  // keep the Windows declarations out; stand-ins follow

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

constexpr uint32_t PAGE_EXECUTE_READWRITE = 0x40;
constexpr uint32_t MEM_COMMIT = 0x1000;
constexpr uint32_t MEM_RESERVE = 0x2000;

namespace {

// Memory below 2 GB, so 32-bit relative jumps reach it and the test binary
// (linked non-PIE, also low), as they always do in a 32-bit process.
void* LowPages(size_t size) {
    void* pages = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    return pages == MAP_FAILED ? nullptr : pages;
}

int g_protectCalls = 0;

}  // namespace

int VirtualProtect(void*, size_t, uint32_t, uint32_t* oldProtect) {
    ++g_protectCalls;
    *oldProtect = PAGE_EXECUTE_READWRITE;
    return 1;
}
void* VirtualAlloc(void*, size_t size, uint32_t, uint32_t) { return LowPages(size); }
int FlushInstructionCache(void*, const void*, size_t) { return 1; }
void* GetCurrentProcess() { return nullptr; }

#include "../src/log.h"
namespace logfile {
void Open() {}
void Write(const char* format, ...) { printf("    log: %s\n", format); }
}  // namespace logfile

#include "../src/hook.cpp"

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

using IntFunction = int (*)();

// push rbp; mov rbp,rsp; nop; nop  |  mov eax,<value>; pop rbp; ret; int3 x5
// The first six bytes are whole, position-independent instructions.
const uint8_t kPrologue[] = {0x55, 0x48, 0x89, 0xE5, 0x90, 0x90};

uint8_t* MakeFunction(int value) {
    uint8_t* code = static_cast<uint8_t*>(LowPages(4096));
    size_t n = 0;
    memcpy(code + n, kPrologue, sizeof(kPrologue));
    n += sizeof(kPrologue);
    code[n++] = 0xB8;
    memcpy(code + n, &value, 4);
    n += 4;
    code[n++] = 0x5D;
    code[n++] = 0xC3;
    memset(code + n, 0xCC, 5);
    return code;
}

hook::Site SiteFor(const char* name, uint8_t* code) {
    return {name, reinterpret_cast<uintptr_t>(code), kPrologue, sizeof(kPrologue), sizeof(kPrologue)};
}

IntFunction g_originalA = nullptr;
int HookA() { return g_originalA() + 1000; }

IntFunction g_originalB = nullptr;
IntFunction g_foreignOriginalB = nullptr;
int ForeignHookB() { return g_foreignOriginalB() * 2; }
int HookB() { return g_originalB() + 1; }

// The extra call is ordinary compiled code: it is free to overwrite the
// registers a function returns its result in.
__attribute__((noinline)) int Clobber() { return 0x7777; }
int g_afterCalls = 0;
void After() {
    ++g_afterCalls;
    volatile int sink = Clobber();
    (void)sink;
}

void TestStockDetour() {
    printf("detour over stock bytes\n");
    uint8_t* code = MakeFunction(42);
    IntFunction function = reinterpret_cast<IntFunction>(code);
    Check(function() == 42, "function works before hooking");

    hook::Result result = hook::Detour(SiteFor("A", code), reinterpret_cast<void*>(HookA),
                                       reinterpret_cast<void**>(&g_originalA));
    Check(result == hook::Result::kInstalled, "reports installed");
    Check(code[0] == 0xE9 && code[5] == 0x90, "site holds a jump and one padding nop");
    Check(code[6] == 0xB8, "bytes after the stolen instructions are untouched");
    Check(function() == 1042, "hook runs and the original still returns 42 through the trampoline");
}

void TestMismatch() {
    printf("unexpected bytes\n");
    uint8_t* code = MakeFunction(7);
    code[3] = 0xEC;  // not the expected prologue any more (and not a jump)
    uint8_t before[16];
    memcpy(before, code, sizeof(before));
    void* original = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1234));
    int protectBefore = g_protectCalls;
    hook::Result result = hook::Detour(SiteFor("mismatch", code), reinterpret_cast<void*>(HookA), &original);
    Check(result == hook::Result::kMismatch, "reports a mismatch");
    Check(memcmp(before, code, sizeof(before)) == 0, "nothing was written");
    Check(g_protectCalls == protectBefore, "memory protection was never touched");
    Check(!hook::IsActive(result), "a mismatch counts as inactive");
}

void TestChainedDetour() {
    printf("detour in front of another library's hook\n");
    uint8_t* code = MakeFunction(7);
    IntFunction function = reinterpret_cast<IntFunction>(code);

    // Another library hooks first, the way detour libraries do: its own
    // trampoline with the stolen bytes, and a jump at the function's start.
    uint8_t* foreignTrampoline = static_cast<uint8_t*>(LowPages(4096));
    memcpy(foreignTrampoline, code, sizeof(kPrologue));
    hook::EmitRelative(foreignTrampoline + sizeof(kPrologue), 0xE9,
                       reinterpret_cast<uintptr_t>(foreignTrampoline + sizeof(kPrologue)),
                       reinterpret_cast<uintptr_t>(code + sizeof(kPrologue)));
    g_foreignOriginalB = reinterpret_cast<IntFunction>(foreignTrampoline);
    hook::EmitRelative(code, 0xE9, reinterpret_cast<uintptr_t>(code), reinterpret_cast<uintptr_t>(ForeignHookB));
    Check(function() == 14, "the other library's hook works on its own (7 * 2)");

    hook::Result result = hook::Detour(SiteFor("B", code), reinterpret_cast<void*>(HookB),
                                       reinterpret_cast<void**>(&g_originalB));
    Check(result == hook::Result::kChained, "reports chained");
    Check(function() == 15, "both hooks run: (7 * 2) + 1");
}

void TestCallBeforeReturn() {
    printf("call before return\n");
    const uint8_t kReturn[] = {0xC3, 0xCC, 0xCC, 0xCC, 0xCC};

    // mov eax,5; ret; int3 padding
    uint8_t* code = static_cast<uint8_t*>(LowPages(4096));
    const uint8_t body[] = {0xB8, 0x05, 0x00, 0x00, 0x00};
    memcpy(code, body, sizeof(body));
    memset(code + sizeof(body), 0xCC, 16);
    code[sizeof(body)] = 0xC3;
    const hook::Site site = {"tail", reinterpret_cast<uintptr_t>(code + sizeof(body)), kReturn, sizeof(kReturn), 0};

    g_afterCalls = 0;
    Check(hook::CallBeforeReturn(site, After) == hook::Result::kInstalled, "reports installed");
    const int first = reinterpret_cast<IntFunction>(code)();
    const int second = reinterpret_cast<IntFunction>(code)();
    Check(g_afterCalls == 2, "the extra call runs once per call of the function, and it returns normally");
    Check(first == 5 && second == 5, "the function's own return value survives the extra call");

    // Same again where another library already replaced the ret with a jump
    // to its own tail (here: mov eax,99; ret).
    uint8_t* chained = static_cast<uint8_t*>(LowPages(4096));
    memcpy(chained, body, sizeof(body));
    memset(chained + sizeof(body), 0xCC, 16);
    uint8_t* foreignTail = static_cast<uint8_t*>(LowPages(4096));
    const uint8_t foreign[] = {0xB8, 0x63, 0x00, 0x00, 0x00, 0xC3};
    memcpy(foreignTail, foreign, sizeof(foreign));
    hook::EmitRelative(chained + sizeof(body), 0xE9, reinterpret_cast<uintptr_t>(chained + sizeof(body)),
                       reinterpret_cast<uintptr_t>(foreignTail));
    const hook::Site chainedSite = {"chained tail", reinterpret_cast<uintptr_t>(chained + sizeof(body)), kReturn,
                                    sizeof(kReturn), 0};

    g_afterCalls = 0;
    Check(hook::CallBeforeReturn(chainedSite, After) == hook::Result::kChained, "reports chained");
    int value = reinterpret_cast<IntFunction>(chained)();
    Check(g_afterCalls == 1, "the extra call runs");
    Check(value == 99, "and the other library's tail still runs afterwards");
}

void TestBackwardJumpTarget() {
    printf("jump decoding\n");
    uint8_t* code = static_cast<uint8_t*>(LowPages(8192));
    uint8_t* jump = code + 4096;
    hook::EmitRelative(jump, 0xE9, reinterpret_cast<uintptr_t>(jump), reinterpret_cast<uintptr_t>(code + 16));
    Check(hook::JumpTarget(jump) == reinterpret_cast<uintptr_t>(code + 16), "a backward jump decodes to its target");
    hook::EmitRelative(jump, 0xE9, reinterpret_cast<uintptr_t>(jump), reinterpret_cast<uintptr_t>(code + 8000));
    Check(hook::JumpTarget(jump) == reinterpret_cast<uintptr_t>(code + 8000), "a forward jump decodes to its target");
}

}  // namespace

int main() {
    TestStockDetour();
    TestMismatch();
    TestChainedDetour();
    TestCallBeforeReturn();
    TestBackwardJumpTarget();
    printf(g_failures == 0 ? "\nall hook engine checks passed\n" : "\n%d hook engine checks FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
