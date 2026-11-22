// A stand-in for the game client, for testing the library without the game.
//
// The library only makes sense inside Wow.exe: it patches four places in the
// client's code and calls a dozen of its routines by address. This program
// plays the client's part. It lays out memory at the client's addresses, puts
// a small routine with the right calling convention (and, where the library
// patches, the client's exact first bytes) at each address the library uses,
// lets the library start up, and then does what the client does: loads the
// UI, calls the registered Lua functions, runs frames, moves the camera. Each
// mock routine records how it was called, and the checks compare that with
// what the client would have to see.
//
// What this proves: the library starts, finds and patches its sites, its
// hooks pass every call through with the stack and the results intact, and
// the features behave as designed. What it cannot prove is that the real
// client behaves like this mock; tools/verify_sites.py and the disassembly
// notes in src/game.h cover the addresses, and the rest needs the game.
//
// All addresses come from src/game.h. See mock_platform.h for the two ways
// this is built and run.

#include "../src/game.h"
#include "mock_platform.h"

namespace {

using platform::Print;

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

int g_checks = 0;
int g_failures = 0;

void PrintNumber(int value) {
    char digits[12];
    int count = 0;
    uint32_t magnitude = value < 0 ? 0u - static_cast<uint32_t>(value) : static_cast<uint32_t>(value);
    do {
        digits[count++] = static_cast<char>('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude != 0);
    char text[13];
    int length = 0;
    if (value < 0) {
        text[length++] = '-';
    }
    while (count > 0) {
        text[length++] = digits[--count];
    }
    text[length] = '\0';
    Print(text);
}

void Section(const char* title) {
    Print(title);
    Print("\n");
}

void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
    }
    Print(ok ? "  ok    " : "  FAIL  ");
    Print(what);
    Print("\n");
}

void CheckEqual(int actual, int expected, const char* what) {
    Check(actual == expected, what);
    if (actual != expected) {
        Print("          got ");
        PrintNumber(actual);
        Print(", expected ");
        PrintNumber(expected);
        Print("\n");
    }
}

// Floats are compared, and on failure shown, in thousandths.
int Thousandths(float value) {
    return static_cast<int>(value * 1000.0f + (value < 0.0f ? -0.5f : 0.5f));
}

void CheckNear(float actual, float expected, const char* what) {
    float difference = actual - expected;
    if (difference < 0.0f) {
        difference = -difference;
    }
    const bool ok = difference < 0.0005f;
    Check(ok, what);
    if (!ok) {
        Print("          got ");
        PrintNumber(Thousandths(actual));
        Print("/1000, expected ");
        PrintNumber(Thousandths(expected));
        Print("/1000\n");
    }
}

bool Same(const char* a, const char* b) {
    while (*a != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

// ---------------------------------------------------------------------------
// Raw memory helpers
// ---------------------------------------------------------------------------

template <typename T>
T& Fixed(uintptr_t address) {
    return *reinterpret_cast<T*>(address);
}

uint32_t Address(const void* pointer) {
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pointer));
}

template <typename Function>
uintptr_t CodeAddress(Function* function) {
    return reinterpret_cast<uintptr_t>(function);
}

uint32_t Bits(float value) {
    uint32_t bits = 0;
    const unsigned char* in = reinterpret_cast<const unsigned char*>(&value);
    unsigned char* out = reinterpret_cast<unsigned char*>(&bits);
    for (int i = 0; i < 4; ++i) {
        out[i] = in[i];
    }
    return bits;
}

// Calls a routine the way the client's own code calls it: `self` in ecx, the
// arguments pushed right to left. Returns what the routine left in eax and
// reports how many bytes of arguments the routine itself took off the stack,
// which is the fingerprint of its calling convention: none for __cdecl, all
// of them for __thiscall. A hook with the wrong signature shows up here. The
// stack pointer is put back whatever the routine did.
__attribute__((noinline)) uint32_t CallClient(uintptr_t routine, void* self, const uint32_t* arguments, int count,
                                              int* popped) {
    uint32_t result;
    uint32_t moved;
    uint32_t scratch;
    __asm__ volatile(
        "movl %%esp, %%esi\n"
        "testl %%edx, %%edx\n"
        "jz 2f\n"
        "1:\n"
        "pushl -4(%%ebx,%%edx,4)\n"
        "decl %%edx\n"
        "jnz 1b\n"
        "2:\n"
        "movl %%esp, %%edi\n"
        "call *%%eax\n"
        "movl %%esp, %%edx\n"
        "subl %%edi, %%edx\n"
        "movl %%esi, %%esp\n"
        : "=a"(result), "=d"(moved), "=c"(scratch)
        : "a"(routine), "c"(self), "b"(arguments), "d"(count)
        : "esi", "edi", "memory", "cc");
    (void)scratch;
    *popped = static_cast<int>(moved);
    return result;
}

// ---------------------------------------------------------------------------
// The mock client: state
// ---------------------------------------------------------------------------

// The objects the library reaches through the client's globals.
alignas(16) uint8_t g_worldFrame[0x8000];
alignas(16) uint8_t g_camera[0x400];
alignas(16) uint8_t g_otherCamera[0x400];
int g_followed = 0;  // what the camera follows; only its address matters

constexpr int kFrameResult = 0x5150;
constexpr int kRegistrationResult = 0x1234;
constexpr uint32_t kCollisionResult = 0x77;
constexpr float kMockYaw = 1.0f;
constexpr float kMockPitch = 0.5f;
constexpr float kMockRoll = 0.25f;
constexpr float kObstacleDistance = 5.0f;  // how far the mock camera can pull back before it "hits" something
constexpr float kMockHeight = 2.0f;

struct FrameCalls {
    int count;
    int arguments[4];
};
FrameCalls g_frame;

struct ClickCalls {
    int captures;
    int performs;
    int lastButton;
    uint8_t* captureFrame;
    uint8_t* performFrame;
    int nextKind;  // what the next capture "finds" under the cursor
};
ClickCalls g_click;

struct MatrixCalls {
    int count;
    void* camera;
    float yaw;
    float pitch;
    float roll;
};
MatrixCalls g_matrix;

struct OrientationCalls {
    int count;
    void* camera;
    void* target;
};
OrientationCalls g_orientation;

struct CollisionCalls {
    int count;
    void* camera;
    float* target;
    float* offset;
    float scale;
    float zoom;
    float zoomBlend;
    uint32_t flags;
};
CollisionCalls g_collision;

int g_uiLoads = 0;

struct Registration {
    const char* name;
    game::lua_CFunction function;
};
Registration g_registered[64];
int g_registeredCount = 0;

// The Lua stack as the library's functions see it.
constexpr int LUA_TBOOLEAN = 1;
struct MockLua {
    int top;
    int types[4];
    double numbers[4];
    int booleans[4];
    int pushes;
    const char* pushedString;
    double pushedNumber;
};
MockLua g_lua;

// ---------------------------------------------------------------------------
// The mock client: routines, each with the real one's calling convention
// (__fastcall with an unused second parameter receives a __thiscall call)
// ---------------------------------------------------------------------------

int __cdecl MockFrameHandler(int a1, int a2, int a3, int a4) {
    ++g_frame.count;
    g_frame.arguments[0] = a1;
    g_frame.arguments[1] = a2;
    g_frame.arguments[2] = a3;
    g_frame.arguments[3] = a4;
    return kFrameResult;
}

void __fastcall MockCapture(uint8_t* worldFrame) {
    ++g_click.captures;
    g_click.captureFrame = worldFrame;
    game::At<int>(worldFrame, game::kWorldFrameHitKind) = g_click.nextKind;
}

int __fastcall MockPerform(uint8_t* worldFrame, void*, int button) {
    ++g_click.performs;
    g_click.performFrame = worldFrame;
    g_click.lastButton = button;
    return 1;
}

void __fastcall MockBuildMatrix(void* camera, void*, float yaw, float pitch, float roll) {
    ++g_matrix.count;
    g_matrix.camera = camera;
    g_matrix.yaw = yaw;
    g_matrix.pitch = pitch;
    g_matrix.roll = roll;
}

void __fastcall MockOrientation(void* camera, void*, void* target, float* yaw, float* pitch, float* roll) {
    ++g_orientation.count;
    g_orientation.camera = camera;
    g_orientation.target = target;
    *yaw = kMockYaw;
    *pitch = kMockPitch;
    *roll = kMockRoll;
}

// Like the real routine: starts from the camera's wanted zoom, and returns it
// untouched if the no-collision flag is set; otherwise something is in the way.
uint32_t __fastcall MockCollision(void* camera, void*, float* target, float* distance, float* height, float* offset,
                                  float scale, float* fraction) {
    ++g_collision.count;
    g_collision.camera = camera;
    g_collision.target = target;
    g_collision.offset = offset;
    g_collision.scale = scale;
    g_collision.zoom = game::At<float>(camera, game::kCameraZoom);
    g_collision.zoomBlend = game::At<float>(camera, game::kCameraZoomBlend);
    g_collision.flags = game::At<uint32_t>(camera, game::kCameraFlags);

    const bool free = (g_collision.flags & game::kCameraFlagNoCollision) != 0;
    *distance = free || g_collision.zoom < kObstacleDistance ? g_collision.zoom : kObstacleDistance;
    *height = kMockHeight;
    *fraction = 1.0f;
    return kCollisionResult;
}

void __cdecl MockRegisterGameFunctions() {
    ++g_uiLoads;
}

void __cdecl MockRegisterLuaFunction(const char* name, game::lua_CFunction function) {
    if (g_registeredCount < 64) {
        g_registered[g_registeredCount].name = name;
        g_registered[g_registeredCount].function = function;
    }
    ++g_registeredCount;
}

bool LuaIndexValid(int index) {
    return index >= 1 && index <= g_lua.top;
}

int __cdecl MockGetTop(game::lua_State*) {
    return g_lua.top;
}

int __cdecl MockType(game::lua_State*, int index) {
    return LuaIndexValid(index) ? g_lua.types[index - 1] : -1;
}

double __cdecl MockToNumber(game::lua_State*, int index) {
    return LuaIndexValid(index) ? g_lua.numbers[index - 1] : 0.0;
}

int __cdecl MockToBoolean(game::lua_State*, int index) {
    return LuaIndexValid(index) ? g_lua.booleans[index - 1] : 0;
}

void __cdecl MockPushNumber(game::lua_State*, double value) {
    ++g_lua.pushes;
    g_lua.pushedNumber = value;
}

void __cdecl MockPushString(game::lua_State*, const char* value) {
    ++g_lua.pushes;
    g_lua.pushedString = value;
}

// ---------------------------------------------------------------------------
// The mock client: machine code at the client's addresses
// ---------------------------------------------------------------------------

constexpr uintptr_t kRegistrationRoutine = game::kLuaRegistrationReturn - game::kLuaRegistrationLength;

void Bytes(uintptr_t& at, const uint8_t* bytes, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        Fixed<uint8_t>(at++) = bytes[i];
    }
}

void Relative(uintptr_t& at, uint8_t opcode, uintptr_t target) {
    const uint32_t displacement = static_cast<uint32_t>(target - (at + 5));
    Fixed<uint8_t>(at++) = opcode;
    for (int shift = 0; shift < 32; shift += 8) {
        Fixed<uint8_t>(at++) = static_cast<uint8_t>(displacement >> shift);
    }
}

void Jump(uintptr_t& at, uintptr_t target) {
    Relative(at, 0xE9, target);
}

void JumpAt(uintptr_t address, uintptr_t target) {
    Jump(address, target);
}

bool Fits(uintptr_t address) {
    return address >= platform::ArenaBegin() && address + 64 <= platform::ArenaEnd();
}

bool AddressesFit() {
    const uintptr_t addresses[] = {
        game::kInWorldFlag,          game::kSpellTargetingPointer, game::kWorldFramePointer,
        game::kCaptureDefaultAction, game::kPerformDefaultAction,  game::kBuildCameraMatrix,
        game::kLuaFunctionRangeMin,  game::kLuaFunctionRangeMax,   game::kRegisterLuaFunction,
        game::kLuaGetTop,            game::kLuaType,               game::kLuaToNumber,
        game::kLuaToBoolean,         game::kLuaPushNumber,         game::kLuaPushString,
        game::kFrameHandler,         game::kCameraOrientation,     game::kCameraCollision,
        kRegistrationRoutine,
    };
    for (uintptr_t address : addresses) {
        if (!Fits(address)) {
            return false;
        }
    }
    return true;
}

void BuildClient() {
    uintptr_t at = 0;

    // The three routines the library detours start with the client's own
    // first instructions (the bytes the library insists on finding), undo
    // them, and carry on in the mock.
    at = game::kFrameHandler;  // push ebp; mov ebp,esp; fld dword [ebp+0x14]
    Bytes(at, game::kFrameHandlerBytes, sizeof(game::kFrameHandlerBytes));
    static const uint8_t kFrameRest[] = {0xDD, 0xD8, 0x5D};  // fstp st(0); pop ebp
    Bytes(at, kFrameRest, sizeof(kFrameRest));
    Jump(at, CodeAddress(MockFrameHandler));

    at = game::kCameraOrientation;  // push ebp; mov ebp,esp; mov eax,[ebp+0xC]
    Bytes(at, game::kCameraOrientationBytes, sizeof(game::kCameraOrientationBytes));
    static const uint8_t kOrientationRest[] = {0x5D};  // pop ebp
    Bytes(at, kOrientationRest, sizeof(kOrientationRest));
    Jump(at, CodeAddress(MockOrientation));

    at = game::kCameraCollision;  // push ebp; mov ebp,esp; sub esp,0x48
    Bytes(at, game::kCameraCollisionBytes, sizeof(game::kCameraCollisionBytes));
    static const uint8_t kCollisionRest[] = {0x8B, 0xE5, 0x5D};  // mov esp,ebp; pop ebp
    Bytes(at, kCollisionRest, sizeof(kCollisionRest));
    Jump(at, CodeAddress(MockCollision));

    // The Lua registration routine: does its work, leaves a value in eax,
    // and ends in the ret-plus-padding the library patches.
    at = kRegistrationRoutine;
    Relative(at, 0xE8, CodeAddress(MockRegisterGameFunctions));  // call
    static const uint8_t kSetResult[] = {0xB8, kRegistrationResult & 0xFF, kRegistrationResult >> 8, 0x00, 0x00};
    Bytes(at, kSetResult, sizeof(kSetResult));  // mov eax, kRegistrationResult
    while (at < game::kLuaRegistrationReturn) {
        Fixed<uint8_t>(at++) = 0x90;  // nop
    }
    Bytes(at, game::kLuaRegistrationReturnBytes, sizeof(game::kLuaRegistrationReturnBytes));

    // Everything the library only calls is a jump to its mock.
    JumpAt(game::kCaptureDefaultAction, CodeAddress(MockCapture));
    JumpAt(game::kPerformDefaultAction, CodeAddress(MockPerform));
    JumpAt(game::kBuildCameraMatrix, CodeAddress(MockBuildMatrix));
    JumpAt(game::kRegisterLuaFunction, CodeAddress(MockRegisterLuaFunction));
    JumpAt(game::kLuaGetTop, CodeAddress(MockGetTop));
    JumpAt(game::kLuaType, CodeAddress(MockType));
    JumpAt(game::kLuaToNumber, CodeAddress(MockToNumber));
    JumpAt(game::kLuaToBoolean, CodeAddress(MockToBoolean));
    JumpAt(game::kLuaPushNumber, CodeAddress(MockPushNumber));
    JumpAt(game::kLuaPushString, CodeAddress(MockPushString));

    // The world: in it, with a world frame whose active camera is g_camera.
    Fixed<uint8_t>(game::kInWorldFlag) = 1;
    Fixed<void*>(game::kSpellTargetingPointer) = nullptr;
    Fixed<uint8_t*>(game::kWorldFramePointer) = g_worldFrame;
    game::At<void*>(g_worldFrame, game::kWorldFrameCamera) = g_camera;
}

// ---------------------------------------------------------------------------
// Doing what the client does
// ---------------------------------------------------------------------------

constexpr uint32_t kCameraFlagUnrelated = 0x100;
constexpr float kClientZoom = 12.0f;
constexpr float kClientZoomBlend = 11.0f;

void ResetCamera(uint8_t* camera) {
    game::At<uint32_t>(camera, game::kCameraFlags) = kCameraFlagUnrelated;
    game::At<float>(camera, game::kCameraZoom) = kClientZoom;
    game::At<float>(camera, game::kCameraZoomBlend) = kClientZoomBlend;
}

game::lua_State* LuaState() {
    return reinterpret_cast<game::lua_State*>(&g_lua);
}

void LuaClear() {
    g_lua.top = 0;
    g_lua.pushes = 0;
    g_lua.pushedString = nullptr;
    g_lua.pushedNumber = 0.0;
}

void LuaPushNumber(double value) {
    g_lua.types[g_lua.top] = game::LUA_TNUMBER;
    g_lua.numbers[g_lua.top] = value;
    g_lua.booleans[g_lua.top] = 1;
    ++g_lua.top;
}

void LuaPushBoolean(bool value) {
    g_lua.types[g_lua.top] = LUA_TBOOLEAN;
    g_lua.numbers[g_lua.top] = 0.0;
    g_lua.booleans[g_lua.top] = value ? 1 : 0;
    ++g_lua.top;
}

const Registration* Find(const char* name) {
    const int count = g_registeredCount < 64 ? g_registeredCount : 64;
    for (int i = count - 1; i >= 0; --i) {  // the latest registration wins, as in Lua
        if (Same(g_registered[i].name, name)) {
            return &g_registered[i];
        }
    }
    return nullptr;
}

// Calls a function the library registered, with the arguments set up through
// LuaPush*. Returns the number of results it reports, or -1 if it is missing.
int CallLua(const char* name) {
    const Registration* registration = Find(name);
    if (registration == nullptr) {
        Check(false, name);
        return -1;
    }
    const uint32_t arguments[] = {Address(LuaState())};
    int popped = -1;
    const uint32_t results = CallClient(CodeAddress(registration->function), nullptr, arguments, 1, &popped);
    if (popped != 0) {
        Check(false, "a registered Lua function left the stack as a __cdecl function must");
    }
    return static_cast<int>(results);
}

void SetCamera(double yaw, double pitch, double distance) {
    LuaClear();
    LuaPushNumber(yaw);
    LuaPushNumber(pitch);
    LuaPushNumber(distance);
    CallLua("ARPG_SetCamera");
}

void SetCameraColliding(double yaw, double pitch, double distance) {
    LuaClear();
    LuaPushNumber(yaw);
    LuaPushNumber(pitch);
    LuaPushNumber(distance);
    LuaPushBoolean(true);
    CallLua("ARPG_SetCamera");
}

void ReleaseCamera() {
    LuaClear();
    CallLua("ARPG_SetCamera");
}

void SetHoldInterval(double milliseconds) {
    LuaClear();
    LuaPushNumber(milliseconds);
    CallLua("ARPG_SetHoldInterval");
}

void Press() {
    LuaClear();
    CallLua("ARPG_HoldStart");
}

void Release() {
    LuaClear();
    CallLua("ARPG_HoldStop");
}

struct Angles {
    float yaw;
    float pitch;
    float roll;
    int popped;
};

Angles Orientation(void* camera, void* target) {
    Angles angles = {-9.0f, -9.0f, -9.0f, -1};
    const uint32_t arguments[] = {Address(target), Address(&angles.yaw), Address(&angles.pitch), Address(&angles.roll)};
    CallClient(game::kCameraOrientation, camera, arguments, 4, &angles.popped);
    return angles;
}

struct Distance {
    float distance;
    float height;
    float fraction;
    uint32_t returned;
    int popped;
};

float g_collisionTarget[3] = {1.0f, 2.0f, 3.0f};
float g_collisionOffset[3] = {0.0f, 0.0f, 0.0f};
constexpr float kCollisionScale = 1.5f;

Distance Collision(void* camera) {
    Distance result = {-9.0f, -9.0f, -9.0f, 0, -1};
    const uint32_t arguments[] = {Address(g_collisionTarget), Address(&result.distance), Address(&result.height),
                                  Address(g_collisionOffset), Bits(kCollisionScale),     Address(&result.fraction)};
    result.returned = CallClient(game::kCameraCollision, camera, arguments, 6, &result.popped);
    return result;
}

constexpr int kFrameArguments[3] = {11, 22, 33};
constexpr float kFrameElapsed = 0.016f;

struct FrameResult {
    uint32_t returned;
    int popped;
};

FrameResult Frame() {
    const uint32_t arguments[] = {static_cast<uint32_t>(kFrameArguments[0]), static_cast<uint32_t>(kFrameArguments[1]),
                                  static_cast<uint32_t>(kFrameArguments[2]), Bits(kFrameElapsed)};
    FrameResult result = {0, -1};
    result.returned = CallClient(game::kFrameHandler, nullptr, arguments, 4, &result.popped);
    return result;
}

// Long enough for a 10 ms repeat interval to have passed on any timer.
void WaitForRepeat() {
    platform::Sleep(60);
}

// ---------------------------------------------------------------------------
// The checks
// ---------------------------------------------------------------------------

constexpr uint8_t kJumpOpcode = 0xE9;
constexpr float kRadiansPerDegree = 0.017453292f;

void TestStartUp() {
    Section("start-up");
    Check(Fixed<uint8_t>(game::kFrameHandler) == kJumpOpcode, "the frame handler is hooked");
    Check(Fixed<uint8_t>(game::kCameraOrientation) == kJumpOpcode, "the camera angle routine is hooked");
    Check(Fixed<uint8_t>(game::kCameraCollision) == kJumpOpcode, "the camera distance routine is hooked");
    Check(Fixed<uint8_t>(game::kLuaRegistrationReturn) == kJumpOpcode, "the Lua registration routine is hooked");
    Check(Fixed<uint32_t>(game::kLuaFunctionRangeMin) == 1 &&
              Fixed<uint32_t>(game::kLuaFunctionRangeMax) == 0xFFFFFFFFu,
          "Lua is allowed to call functions outside the client");
    CheckEqual(g_registeredCount, 0, "nothing is registered before a UI exists");
    CheckEqual(g_uiLoads, 0, "none of the client's routines has run yet");
}

void TestUiLoad() {
    Section("UI load");
    int popped = -1;
    const uint32_t result = CallClient(kRegistrationRoutine, nullptr, nullptr, 0, &popped);
    CheckEqual(g_uiLoads, 1, "the client's own registration still runs");
    CheckEqual(static_cast<int>(result), kRegistrationResult, "and still returns what it returned");
    CheckEqual(popped, 0, "with the stack as it left it before");
    CheckEqual(g_registeredCount, 5, "the library registers five Lua functions");

    const char* const names[] = {"ARPG_ClientVersion", "ARPG_HoldStart", "ARPG_HoldStop", "ARPG_SetHoldInterval",
                                 "ARPG_SetCamera"};
    const uint32_t low = Fixed<uint32_t>(game::kLuaFunctionRangeMin);
    const uint32_t high = Fixed<uint32_t>(game::kLuaFunctionRangeMax);
    bool allCallable = true;
    for (const char* name : names) {
        const Registration* registration = Find(name);
        Check(registration != nullptr, name);
        if (registration != nullptr) {
            // The client's own test before it calls a C function from Lua.
            const uint32_t address = static_cast<uint32_t>(CodeAddress(registration->function));
            allCallable = allCallable && address >= low && address < high;
        }
    }
    Check(allCallable, "each passes the client's function-address test");
}

void TestVersion() {
    Section("ARPG_ClientVersion");
    LuaClear();
    CheckEqual(CallLua("ARPG_ClientVersion"), 2, "returns two values");
    CheckEqual(g_lua.pushes, 2, "pushed as two values");
    const bool hasVersion = g_lua.pushedString != nullptr && g_lua.pushedString[0] != '\0';
    Check(hasVersion, "a version string");
    if (hasVersion) {
        Print("          version ");
        Print(g_lua.pushedString);
        Print("\n");
    }
    CheckEqual(static_cast<int>(g_lua.pushedNumber), 15, "and the feature bits: all four parts working");
}

void TestCamera() {
    Section("camera, not locked");
    ResetCamera(g_camera);
    ResetCamera(g_otherCamera);
    g_orientation = {};
    g_collision = {};
    g_matrix = {};

    Angles angles = Orientation(g_camera, &g_followed);
    CheckEqual(g_orientation.count, 1, "the client's angle routine runs");
    Check(g_orientation.camera == g_camera && g_orientation.target == &g_followed, "with the camera and target it was given");
    CheckNear(angles.yaw, kMockYaw, "its yaw comes back untouched");
    CheckNear(angles.pitch, kMockPitch, "its pitch comes back untouched");
    CheckNear(angles.roll, kMockRoll, "its roll comes back untouched");
    CheckEqual(angles.popped, 16, "four arguments leave the stack (__thiscall)");
    CheckEqual(g_matrix.count, 0, "the matrix is not rebuilt");

    Distance distance = Collision(g_camera);
    CheckEqual(g_collision.count, 1, "the client's distance routine runs");
    Check(g_collision.camera == g_camera && g_collision.target == g_collisionTarget &&
              g_collision.offset == g_collisionOffset,
          "with the camera and vectors it was given");
    CheckNear(g_collision.scale, kCollisionScale, "and the float argument intact");
    CheckNear(g_collision.zoom, kClientZoom, "it sees the client's own zoom");
    CheckNear(distance.distance, kObstacleDistance, "its (colliding) distance comes back untouched");
    CheckNear(distance.height, kMockHeight, "its height comes back");
    CheckNear(distance.fraction, 1.0f, "its last result comes back");
    CheckEqual(static_cast<int>(distance.returned), static_cast<int>(kCollisionResult), "its return value comes back");
    CheckEqual(distance.popped, 24, "six arguments leave the stack (__thiscall)");

    Section("camera, locked: ARPG_SetCamera(0, 55, 30)");
    SetCamera(0.0, 55.0, 30.0);
    g_matrix = {};
    angles = Orientation(g_camera, &g_followed);
    CheckEqual(g_orientation.count, 2, "the client's angle routine still runs first");
    CheckNear(angles.yaw, 0.0f, "yaw is the locked one: north");
    CheckNear(angles.pitch, 55.0f * kRadiansPerDegree, "pitch is the locked one, in radians");
    CheckNear(angles.roll, kMockRoll, "roll stays the client's");
    CheckEqual(angles.popped, 16, "the stack is as the client expects");
    CheckEqual(g_matrix.count, 1, "the matrix is rebuilt once");
    Check(g_matrix.camera == g_camera, "for this camera");
    CheckNear(g_matrix.yaw, 0.0f, "with the locked yaw");
    CheckNear(g_matrix.pitch, 55.0f * kRadiansPerDegree, "the locked pitch");
    CheckNear(g_matrix.roll, kMockRoll, "and the client's roll");

    distance = Collision(g_camera);
    CheckNear(g_collision.zoom, 30.0f, "the distance routine is shown the locked distance");
    CheckNear(g_collision.zoomBlend, 30.0f, "in both zoom fields");
    Check((g_collision.flags & game::kCameraFlagNoCollision) != 0, "and told not to collide");
    Check((g_collision.flags & kCameraFlagUnrelated) != 0, "with the camera's other flags left alone");
    CheckNear(distance.distance, 30.0f, "the camera sits at the locked distance, through the obstacle");
    CheckEqual(static_cast<int>(distance.returned), static_cast<int>(kCollisionResult), "the return value comes back");
    CheckEqual(distance.popped, 24, "the stack is as the client expects");
    CheckNear(game::At<float>(g_camera, game::kCameraZoom), kClientZoom, "afterwards the client's zoom is back");
    CheckNear(game::At<float>(g_camera, game::kCameraZoomBlend), kClientZoomBlend, "and its zoom blend");
    CheckEqual(static_cast<int>(game::At<uint32_t>(g_camera, game::kCameraFlags)), static_cast<int>(kCameraFlagUnrelated),
               "and its flags");

    Section("camera, locked with collision: ARPG_SetCamera(90, 55, 30, true)");
    SetCameraColliding(90.0, 55.0, 30.0);
    angles = Orientation(g_camera, &g_followed);
    CheckNear(angles.yaw, 90.0f * kRadiansPerDegree, "yaw is given in degrees");
    distance = Collision(g_camera);
    Check((g_collision.flags & game::kCameraFlagNoCollision) == 0, "the distance routine may collide");
    CheckNear(g_collision.zoom, 30.0f, "starting from the locked distance");
    CheckNear(distance.distance, kObstacleDistance, "so the camera stops at the obstacle");

    Section("camera, limits: ARPG_SetCamera(0, 120, 500)");
    SetCamera(0.0, 120.0, 500.0);
    angles = Orientation(g_camera, &g_followed);
    CheckNear(angles.pitch, 89.0f * kRadiansPerDegree, "pitch stops short of straight down");
    distance = Collision(g_camera);
    CheckNear(distance.distance, 200.0f, "distance stops at 200");

    Section("camera, locked, but not ours to move");
    SetCamera(0.0, 55.0, 30.0);
    game::At<uint32_t>(g_camera, game::kCameraFlags) |= game::kCameraFlagModelDriven;
    angles = Orientation(g_camera, &g_followed);
    CheckNear(angles.yaw, kMockYaw, "a camera driven by a model animation keeps its angles");
    distance = Collision(g_camera);
    CheckNear(distance.distance, kObstacleDistance, "and its distance");
    ResetCamera(g_camera);

    angles = Orientation(g_otherCamera, &g_followed);
    CheckNear(angles.yaw, kMockYaw, "a camera that is not the active one keeps its angles");
    distance = Collision(g_otherCamera);
    CheckNear(distance.distance, kObstacleDistance, "and its distance");

    angles = Orientation(g_camera, nullptr);
    CheckNear(angles.yaw, kMockYaw, "a camera that follows nothing keeps its angles");

    game::At<uint32_t>(g_camera, game::kCameraFlags) |= game::kCameraFlagNoCollision;
    Collision(g_camera);
    Check((game::At<uint32_t>(g_camera, game::kCameraFlags) & game::kCameraFlagNoCollision) != 0,
          "a no-collision flag the client set itself survives");
    ResetCamera(g_camera);

    Section("camera, released: ARPG_SetCamera()");
    ReleaseCamera();
    g_matrix = {};
    angles = Orientation(g_camera, &g_followed);
    CheckNear(angles.yaw, kMockYaw, "the client's yaw is back");
    CheckNear(angles.pitch, kMockPitch, "and its pitch");
    CheckEqual(g_matrix.count, 0, "the matrix is not rebuilt");
    distance = Collision(g_camera);
    CheckNear(g_collision.zoom, kClientZoom, "the distance routine sees the client's zoom again");
    CheckNear(distance.distance, kObstacleDistance, "and collides again");
}

void TestHoldMove() {
    Section("frames with nothing held");
    g_frame = {};
    g_click = {};
    g_click.nextKind = game::kHitTerrain;
    FrameResult frame = Frame();
    CheckEqual(g_frame.count, 1, "the client's frame handler runs");
    Check(g_frame.arguments[0] == kFrameArguments[0] && g_frame.arguments[1] == kFrameArguments[1] &&
              g_frame.arguments[2] == kFrameArguments[2] &&
              static_cast<uint32_t>(g_frame.arguments[3]) == Bits(kFrameElapsed),
          "with its four arguments intact");
    CheckEqual(static_cast<int>(frame.returned), kFrameResult, "its return value comes back");
    CheckEqual(frame.popped, 0, "the arguments stay on the stack (__cdecl)");
    CheckEqual(g_click.captures, 0, "nothing looks under the cursor");

    Section("press on the ground: ARPG_HoldStart()");
    SetHoldInterval(2000.0);  // the longest allowed, so the next frame cannot count as a repeat
    Press();
    CheckEqual(g_click.captures, 0, "the Lua call itself does nothing in the world");
    Frame();
    CheckEqual(g_frame.count, 2, "the frame handler still runs");
    CheckEqual(g_click.captures, 1, "the next frame looks under the cursor");
    Check(g_click.captureFrame == g_worldFrame, "in the client's world frame");
    CheckEqual(g_click.performs, 1, "and acts once");
    CheckEqual(g_click.lastButton, game::kClickRight, "as a right click: click-to-move");
    Check(g_click.performFrame == g_worldFrame, "on the client's world frame");
    Frame();
    CheckEqual(g_click.captures, 1, "the frame after that does nothing: the repeat interval has not passed");

    Section("holding");
    SetHoldInterval(10.0);
    WaitForRepeat();
    Frame();
    CheckEqual(g_click.performs, 2, "once the interval has passed, the move is issued again");
    g_click.nextKind = game::kHitWorldObject;
    WaitForRepeat();
    Frame();
    CheckEqual(g_click.performs, 3, "a building under the cursor counts as ground");
    g_click.nextKind = game::kHitObject;
    WaitForRepeat();
    Frame();
    CheckEqual(g_click.captures, 4, "over a unit the cursor is still followed");
    CheckEqual(g_click.performs, 3, "but a held button does not click the unit again and again");
    g_click.nextKind = game::kHitNothing;
    WaitForRepeat();
    Frame();
    CheckEqual(g_click.performs, 3, "and nothing under the cursor means nothing to do");

    Section("release: ARPG_HoldStop()");
    Release();
    g_click.nextKind = game::kHitTerrain;
    WaitForRepeat();
    Frame();
    CheckEqual(g_click.captures, 5, "after the release nothing looks under the cursor any more");
    CheckEqual(g_click.performs, 3, "and nothing moves");

    Section("other presses");
    g_click.nextKind = game::kHitObject;
    Press();
    Frame();
    CheckEqual(g_click.performs, 4, "a press on a unit acts on it once");
    CheckEqual(g_click.lastButton, game::kClickRight, "as a right click: interact, attack, loot");
    Release();

    g_click.nextKind = game::kHitNothing;
    Press();
    Frame();
    CheckEqual(g_click.performs, 4, "a press on nothing does nothing");
    Release();

    g_click.nextKind = game::kHitTerrain;
    Press();
    Release();
    Frame();
    CheckEqual(g_click.performs, 5, "a click shorter than a frame still moves once");
    WaitForRepeat();
    Frame();
    CheckEqual(g_click.performs, 5, "and only once");

    Section("a spell waiting for its target");
    Fixed<void*>(game::kSpellTargetingPointer) = &g_followed;
    Press();
    Frame();
    CheckEqual(g_click.performs, 6, "the press goes to the spell");
    CheckEqual(g_click.lastButton, game::kClickLeft, "as a left click: place it or pick the unit");
    WaitForRepeat();
    Frame();
    CheckEqual(g_click.performs, 6, "holding on does not walk off or cancel the spell");
    Release();
    Fixed<void*>(game::kSpellTargetingPointer) = nullptr;

    Section("outside the world");
    int captures = g_click.captures;
    Press();
    Fixed<uint8_t>(game::kInWorldFlag) = 0;
    Frame();
    CheckEqual(g_click.captures, captures, "on a loading screen nothing is touched");
    Fixed<uint8_t>(game::kInWorldFlag) = 1;
    WaitForRepeat();
    Frame();
    CheckEqual(g_click.captures, captures, "and the held button is forgotten, not resumed");

    Press();
    Fixed<uint8_t*>(game::kWorldFramePointer) = nullptr;
    Frame();
    CheckEqual(g_click.captures, captures, "without a world frame nothing is touched");
    Fixed<uint8_t*>(game::kWorldFramePointer) = g_worldFrame;
    Release();
}

void TestUiReload() {
    Section("UI reload");
    SetCamera(0.0, 55.0, 30.0);
    g_click.nextKind = game::kHitTerrain;
    Press();
    const int captures = g_click.captures;

    int popped = -1;
    CallClient(kRegistrationRoutine, nullptr, nullptr, 0, &popped);
    CheckEqual(g_uiLoads, 2, "the client registers its functions again");
    CheckEqual(g_registeredCount, 10, "and so does the library");

    const Angles angles = Orientation(g_camera, &g_followed);
    CheckNear(angles.yaw, kMockYaw, "the camera lock is dropped until the new UI asks for it");
    WaitForRepeat();
    Frame();
    CheckEqual(g_click.captures, captures, "and so is the held button");
}

}  // namespace

int MockMain() {
    Print("WotLK-ARPG client library against a mock client\n\n");
    if (!platform::PrepareArena()) {
        Print("FAIL  could not set up the memory that stands in for the client\n");
        return 2;
    }
    if (!AddressesFit()) {
        Print("FAIL  the stand-in memory does not span the client's addresses\n");
        return 2;
    }
    BuildClient();
    if (!platform::LoadLibraryUnderTest()) {
        Print("FAIL  the library did not load\n");
        return 1;
    }

    TestStartUp();
    TestUiLoad();
    TestVersion();
    TestCamera();
    TestHoldMove();
    TestUiReload();

    Print("\n");
    PrintNumber(g_checks);
    Print(" checks, ");
    PrintNumber(g_failures);
    Print(g_failures == 0 ? " failed: all passed\n" : " FAILED\n");
    return g_failures == 0 ? 0 : 1;
}
