// A stand-in for the game client, for testing the library without the game.
//
// The library only makes sense inside Wow.exe: it patches eight places in the
// client's code and calls two dozen of its routines by address. This program
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
constexpr uint32_t kCollisionResult = 0x7F;  // includes the first-person bit, to see who lets it through
uint32_t g_collisionReturn = kCollisionResult;

// The mock camera's "matrix": just the angles it was last built from, kept
// where the real camera keeps its matrix.
constexpr size_t kMockMatrixYaw = 0x14;
constexpr size_t kMockMatrixPitch = 0x18;
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

// What the next capture "finds" under the cursor.
struct CursorScene {
    int kind;
    game::Vec3 hit;
    game::Vec3 rayStart;
    game::Vec3 rayEnd;
    uint32_t guidLow;
    uint32_t guidHigh;
};
CursorScene g_cursor;

struct ClickCalls {
    int captures;
    int performs;
    int lastButton;
    uint8_t* captureFrame;
    uint8_t* performFrame;
    game::Vec3 performPosition;  // the hit position the click handler would have acted on
};
ClickCalls g_click;

struct TerrainClicks {
    int count;
    game::TerrainClick last;
};
TerrainClicks g_terrain;

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

// The player's character: an object whose virtual table has the position
// routine in the client's slot.
constexpr uint64_t kMockPlayerGuid = 0x0000000700001234ull;
uint64_t g_playerGuid = kMockPlayerGuid;  // zero: not in the world
game::Vec3 g_playerPosition = {100.0f, 200.0f, 10.0f};
void* g_playerVirtuals[32];
struct MockObject {
    void** virtuals;
};
MockObject g_player = {g_playerVirtuals};
uint32_t g_lookupTypeMask = 0;

// The Lua stack as the library's functions see it.
constexpr int LUA_TBOOLEAN = 1;
struct MockLua {
    int top;
    int types[4];
    double numbers[4];
    int booleans[4];
    int pushes;
    const char* pushedString;
    double pushedNumbers[12];
    int pushedNumberCount;
};
MockLua g_lua;

// The graphics device: an object whose virtual table has the set-projection
// routine in the client's slot, and the matrices at the client's offsets.
alignas(16) uint8_t g_device[0x2000];
void* g_deviceVirtuals[64];

struct DeviceCalls {
    int setProjection;
    int shaderUpdates;
    game::Matrix shaderProjection;  // what the vertex shaders were last handed
};
DeviceCalls g_deviceCalls;

// The Direct3D device rescales depth when it is given a projection. Another
// renderer does something else; switching this off stands in for one.
bool g_deviceRescalesDepth = true;

// The world scene.
bool g_outdoors = true;             // what the client's own test says about the character
void* g_outdoorsAskedOf = nullptr;
int g_locateCalls = 0;
void* g_cameraStandsIn = nullptr;   // the building the client's routine finds the camera in, if any

struct Visit {
    void* building;
    uint32_t index;
    uint32_t interiorPath;
};
constexpr int kMaxVisits = 32;
Visit g_visits[kMaxVisits];
int g_visitCount = 0;
int g_visitsOfUnreadyGroups = 0;  // each of these would have crashed the real client

struct StageCalls {
    int count;
    int visitsSeen;           // visits made by the time the client's routine ran
    game::Matrix projection;  // the device's projection at that moment
};
StageCalls g_finishVisits;
StageCalls g_beforeDrawing;

int g_viewTests = 0;
game::Box g_viewBox = {{40.0f, 140.0f, -200.0f}, {160.0f, 260.0f, 200.0f}};  // what is "in view"

// Solid level slabs for lines of sight to run into: a ceiling, an archway, a
// gallery, and the floor the character stands on. Each reaches from far south
// up to `north` (x), so one can be put in the way of the camera without being
// overhead.
struct Slab {
    bool present;
    float z0;
    float z1;
    float north;
};
constexpr float kEverywhere = 1e9f;
constexpr int kSlabs = 3;
Slab g_slabs[kSlabs] = {{false, 0.0f, 0.0f, kEverywhere},
                        {false, 0.0f, 0.0f, kEverywhere},
                        {true, 9.0f, 10.0f, kEverywhere}};  // the floor: its top is where the feet are
struct IntersectCalls {
    int count;
    int upward;
    int downward;
    uint32_t flags;
    uint32_t last;
};
IntersectCalls g_intersect;
bool g_intersectLies = false;             // report a hit point that is not where "how far along" says
bool g_intersectCountsFromFarEnd = false;  // report the last thing on the line, not the first

// The cursor's ray as the client's routine reports it.
struct RayCalls {
    int count;
    uint8_t* worldFrame;
    float x;
    float y;
};
RayCalls g_rayCalls;
bool g_rayFound = true;
game::Vec3 g_rayStart;
game::Vec3 g_rayEnd;
uint32_t g_rayArguments[4];  // what the mock hit test passes: x, y, start, end
uint8_t* g_rayThis = nullptr;
uint32_t g_rayResult = 0;

// Buildings, laid out at the client's offsets.
constexpr int kMaxMockGroups = 8;
struct MockBuilding {
    alignas(4) uint8_t building[0x140];
    alignas(4) uint8_t model[0x1F8 + 4 * kMaxMockGroups];
    alignas(4) uint8_t groupInfo[game::kModelGroupInfoSize * kMaxMockGroups];
    alignas(4) uint8_t groupData[kMaxMockGroups][0x1A0];
    alignas(4) uint8_t placed[kMaxMockGroups][0xC0];
    alignas(4) uint8_t links[kMaxMockGroups][0x20];
    void* array[kMaxMockGroups];
};
constexpr int kMockBuildings = 4;
MockBuilding g_buildings[kMockBuildings];
constexpr int32_t kBuildingListLinkOffset = 0x10;  // where this mock keeps a building's list link
constexpr int32_t kGroupListLinkOffset = 0x0C;
constexpr uintptr_t kListEnd = 1;                  // the client ends its lists with an odd value

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
    game::At<int>(worldFrame, game::kWorldFrameHitKind) = g_cursor.kind;
    game::At<uint32_t>(worldFrame, game::kWorldFrameHitGuid) = g_cursor.guidLow;
    game::At<uint32_t>(worldFrame, game::kWorldFrameHitGuid + 4) = g_cursor.guidHigh;
    game::At<game::Vec3>(worldFrame, game::kWorldFrameHitPosition) = g_cursor.hit;
    game::At<game::Vec3>(worldFrame, game::kWorldFrameRayStart) = g_cursor.rayStart;
    game::At<game::Vec3>(worldFrame, game::kWorldFrameRayEnd) = g_cursor.rayEnd;
}

int __fastcall MockPerform(uint8_t* worldFrame, void*, int button) {
    ++g_click.performs;
    g_click.performFrame = worldFrame;
    g_click.lastButton = button;
    g_click.performPosition = game::At<game::Vec3>(worldFrame, game::kWorldFrameHitPosition);
    return 1;
}

int __cdecl MockTerrainClick(const game::TerrainClick* click) {
    ++g_terrain.count;
    g_terrain.last = *click;
    return 1;
}

uint64_t __cdecl MockPlayerGuid() {
    return g_playerGuid;
}

void* __cdecl MockObjectByGuid(uint32_t low, uint32_t high, uint32_t typeMask, const char* file, int) {
    g_lookupTypeMask = typeMask;
    const uint64_t guid = (static_cast<uint64_t>(high) << 32) | low;
    return guid == kMockPlayerGuid && file != nullptr ? &g_player : nullptr;
}

game::Vec3* __fastcall MockGetPosition(void*, void*, game::Vec3* out) {
    *out = g_playerPosition;
    return out;
}

void __fastcall MockBuildMatrix(void* camera, void*, float yaw, float pitch, float roll) {
    ++g_matrix.count;
    g_matrix.camera = camera;
    g_matrix.yaw = yaw;
    g_matrix.pitch = pitch;
    g_matrix.roll = roll;
    game::At<float>(camera, kMockMatrixYaw) = yaw;
    game::At<float>(camera, kMockMatrixPitch) = pitch;
}

// Like the real routine: reports the client's own angles and, if the camera
// follows something, builds the matrix from them.
void __fastcall MockOrientation(void* camera, void*, void* target, float* yaw, float* pitch, float* roll) {
    ++g_orientation.count;
    g_orientation.camera = camera;
    g_orientation.target = target;
    *yaw = kMockYaw;
    *pitch = kMockPitch;
    *roll = kMockRoll;
    if (target != nullptr) {
        game::At<float>(camera, kMockMatrixYaw) = kMockYaw;
        game::At<float>(camera, kMockMatrixPitch) = kMockPitch;
    }
}

// Like the real routine: starts from the larger of the camera's zoom fields,
// and returns that untouched if the first-person flag is set; otherwise
// something is in the way.
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

    const float wanted = g_collision.zoom > g_collision.zoomBlend ? g_collision.zoom : g_collision.zoomBlend;
    const bool firstPerson = (g_collision.flags & game::kCameraFlagFirstPerson) != 0;
    *distance = firstPerson || wanted < kObstacleDistance ? wanted : kObstacleDistance;
    *height = kMockHeight;
    *fraction = 1.0f;
    return g_collisionReturn;
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
    if (g_lua.pushedNumberCount < 12) {
        g_lua.pushedNumbers[g_lua.pushedNumberCount++] = value;
    }
}

void __cdecl MockPushString(game::lua_State*, const char* value) {
    ++g_lua.pushes;
    g_lua.pushedString = value;
}

// Like the Direct3D device's routine (0x006A9B40): keeps the matrix, and
// derives the hardware one by working out the near and far distances from
// two of its elements and building Direct3D's depth terms from those.
void __fastcall MockSetProjection(uint8_t* device, void*, const game::Matrix* projection) {
    ++g_deviceCalls.setProjection;
    game::At<game::Matrix>(device, game::kGxDeviceProjection) = *projection;
    game::Matrix hardware = *projection;
    if (g_deviceRescalesDepth && hardware.m[3][3] == 0.0f) {
        const float c2 = hardware.m[2][2];
        const float d2 = hardware.m[3][2];
        const float nearDistance = -d2 / (c2 + 1.0f);
        const float farDistance = -d2 / (c2 - 1.0f);
        hardware.m[2][2] = farDistance / (farDistance - nearDistance);
        hardware.m[3][2] = farDistance * nearDistance / (nearDistance - farDistance);
    }
    game::At<game::Matrix>(device, game::kGxDeviceHardwareProjection) = hardware;
}

void __cdecl MockUpdateShaderProjection() {
    ++g_deviceCalls.shaderUpdates;
    g_deviceCalls.shaderProjection = game::At<game::Matrix>(g_device, game::kGxDeviceHardwareProjection);
}

bool __fastcall MockUnitIsOutdoors(void* unit) {
    g_outdoorsAskedOf = unit;
    return g_outdoors;
}

// Like the real routine: clears what it found last time, then looks.
void __cdecl MockLocateCamera() {
    ++g_locateCalls;
    Fixed<void*>(game::kSceneCameraBuilding) = g_cameraStandsIn;
    Fixed<void*>(game::kSceneCameraBuildingSecond) = nullptr;
    Fixed<uint32_t>(game::kSceneCameraGroupCount) = g_cameraStandsIn != nullptr ? 1 : 0;
    Fixed<uint32_t>(game::kSceneCameraGroupCountSecond) = 0;
    Fixed<char>(game::kSceneCameraBuildingName) = g_cameraStandsIn != nullptr ? 'w' : '\0';
    Fixed<char>(game::kSceneCameraGroupName) = g_cameraStandsIn != nullptr ? 'g' : '\0';
}

// Like the real routine, this looks the placed group up by index and uses
// the group's own data without checking either. Where the real one would
// crash, this one counts.
void __cdecl MockVisit(uint32_t index, void* buildingPointer) {
    uint8_t* building = static_cast<uint8_t*>(buildingPointer);
    const int32_t mode = game::At<int32_t>(building, game::kBuildingGroupArrayMode);
    void* placed = mode == -1 ? game::At<void**>(building, game::kBuildingGroupArrayData)[index]
                              : game::At<void*>(building, game::kBuildingGroupArray + index * sizeof(void*));
    uint8_t* model = game::At<uint8_t*>(building, game::kBuildingModel);
    bool ready = placed != nullptr && game::At<uint32_t>(model, game::kModelLoaded) != 0;
    if (ready) {
        uint8_t* data = game::At<uint8_t*>(model, game::kModelGroups + index * sizeof(void*));
        ready = (game::At<uint32_t>(data, game::kGroupDataLoadFlags) & game::kGroupDataLoaded) != 0;
    }
    if (!ready) {
        ++g_visitsOfUnreadyGroups;
        return;
    }
    if (g_visitCount < kMaxVisits) {
        g_visits[g_visitCount] = {building, index, Fixed<uint32_t>(game::kSceneInteriorPath)};
    }
    ++g_visitCount;
}

int __cdecl MockBoxOutsideView(const game::Box* box) {
    ++g_viewTests;
    const bool apart = box->high.x < g_viewBox.low.x || box->low.x > g_viewBox.high.x ||
                       box->high.y < g_viewBox.low.y || box->low.y > g_viewBox.high.y ||
                       box->high.z < g_viewBox.low.z || box->low.z > g_viewBox.high.z;
    return apart ? 1 : 0;
}

void __cdecl MockFinishVisits() {
    ++g_finishVisits.count;
    g_finishVisits.visitsSeen = g_visitCount;
    g_finishVisits.projection = game::At<game::Matrix>(g_device, game::kGxDeviceProjection);
}

void __cdecl MockBeforeDrawing() {
    ++g_beforeDrawing.count;
    g_beforeDrawing.visitsSeen = g_visitCount;
    g_beforeDrawing.projection = game::At<game::Matrix>(g_device, game::kGxDeviceProjection);
}

// Where the line goes through the face of a slab: the first such place
// counted from `from`, as the client's routine reports it (or the last, for
// a routine that counts from the other end).
bool __cdecl MockIntersect(const game::Vec3* from, const game::Vec3* to, game::Vec3* hit, float* fraction,
                           uint32_t flags, uint32_t last) {
    ++g_intersect.count;
    ++(to->z > from->z ? g_intersect.upward : g_intersect.downward);
    g_intersect.flags = flags;
    g_intersect.last = last;
    if (*fraction != 1.0f) {
        return false;
    }
    bool any = false;
    float best = 0.0f;
    float bestFace = 0.0f;
    for (const Slab& slab : g_slabs) {
        if (!slab.present) {
            continue;
        }
        const float faces[2] = {slab.z0, slab.z1};
        for (float face : faces) {
            const bool through = (from->z < face && to->z > face) || (from->z > face && to->z < face);
            if (!through) {
                continue;
            }
            const float along = (face - from->z) / (to->z - from->z);
            if (from->x + (to->x - from->x) * along > slab.north) {
                continue;
            }
            if (any && (g_intersectCountsFromFarEnd ? along <= best : along >= best)) {
                continue;
            }
            any = true;
            best = along;
            bestFace = face;
        }
    }
    if (!any) {
        return false;
    }
    hit->x = from->x + (to->x - from->x) * best;
    hit->y = from->y + (to->y - from->y) * best;
    hit->z = g_intersectLies ? bestFace + 5.0f : bestFace;
    *fraction = best;
    return true;
}

int __fastcall MockCursorRay(uint8_t* worldFrame, void*, float x, float y, game::Vec3* start, game::Vec3* end) {
    ++g_rayCalls.count;
    g_rayCalls.worldFrame = worldFrame;
    g_rayCalls.x = x;
    g_rayCalls.y = y;
    if (!g_rayFound) {
        return 0;
    }
    *start = g_rayStart;
    *end = g_rayEnd;
    return 1;
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

// Where a `call rel32` with these bytes, sitting at `site`, goes.
constexpr uintptr_t CallTarget(uintptr_t site, const uint8_t* bytes) {
    const uint32_t displacement = static_cast<uint32_t>(bytes[1]) | static_cast<uint32_t>(bytes[2]) << 8 |
                                  static_cast<uint32_t>(bytes[3]) << 16 | static_cast<uint32_t>(bytes[4]) << 24;
    return site + 5 + displacement;
}

constexpr uintptr_t kLocateCameraRoutine = CallTarget(game::kLocateCameraCall, game::kLocateCameraCallBytes);
constexpr uintptr_t kFinishVisitsRoutine = CallTarget(game::kFinishVisitsCall, game::kFinishVisitsCallBytes);
constexpr uintptr_t kBeforeDrawingRoutine = CallTarget(game::kBeforeDrawingCall, game::kBeforeDrawingCallBytes);
constexpr uintptr_t kCursorRayRoutine = CallTarget(game::kCursorRayCall, game::kCursorRayCallBytes);

// The mock's cut-down cursor hit test: thirty bytes that put the arguments
// in place, so that its call lands exactly on the client's address for it.
constexpr uintptr_t kCursorHitTest = game::kCursorRayCall - 30;

void Word(uintptr_t& at, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        Fixed<uint8_t>(at++) = static_cast<uint8_t>(value >> shift);
    }
}

bool AddressesFit() {
    const uintptr_t addresses[] = {
        game::kInWorldFlag,          game::kSpellTargetingPointer, game::kWorldFramePointer,
        game::kCaptureDefaultAction, game::kPerformDefaultAction,  game::kBuildCameraMatrix,
        game::kLuaFunctionRangeMin,  game::kLuaFunctionRangeMax,   game::kRegisterLuaFunction,
        game::kLuaGetTop,            game::kLuaType,               game::kLuaToNumber,
        game::kLuaToBoolean,         game::kLuaPushNumber,         game::kLuaPushString,
        game::kFrameHandler,         game::kCameraOrientation,     game::kCameraCollision,
        game::kPlayerGuid,           game::kObjectByGuid,          game::kTerrainClick,
        kRegistrationRoutine,
        game::kGxDevicePointer,      game::kUpdateShaderProjection, game::kSceneCameraPosition,
        game::kSceneCameraBuilding,  game::kSceneCameraBuildingSecond, game::kSceneCameraGroupCount,
        game::kSceneCameraGroupCountSecond, game::kSceneCameraBuildingName, game::kSceneCameraGroupName,
        game::kSceneFrustumLevel,    game::kSceneInteriorPath,     game::kPlacedBuildingsHead,
        game::kPlacedBuildingsLinkOffset, game::kVisitBuildingGroup, game::kBoxOutsideView,
        game::kIntersectWorld,       game::kUnitIsOutdoors,        game::kLocateCameraCall,
        game::kFinishVisitsCall,     game::kBeforeDrawingCall,     kCursorHitTest,
        kLocateCameraRoutine,        kFinishVisitsRoutine,         kBeforeDrawingRoutine,
        kCursorRayRoutine,
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
    JumpAt(game::kTerrainClick, CodeAddress(MockTerrainClick));
    JumpAt(game::kPlayerGuid, CodeAddress(MockPlayerGuid));
    JumpAt(game::kObjectByGuid, CodeAddress(MockObjectByGuid));
    JumpAt(game::kBuildCameraMatrix, CodeAddress(MockBuildMatrix));
    JumpAt(game::kRegisterLuaFunction, CodeAddress(MockRegisterLuaFunction));
    JumpAt(game::kLuaGetTop, CodeAddress(MockGetTop));
    JumpAt(game::kLuaType, CodeAddress(MockType));
    JumpAt(game::kLuaToNumber, CodeAddress(MockToNumber));
    JumpAt(game::kLuaToBoolean, CodeAddress(MockToBoolean));
    JumpAt(game::kLuaPushNumber, CodeAddress(MockPushNumber));
    JumpAt(game::kLuaPushString, CodeAddress(MockPushString));
    g_playerVirtuals[game::kObjectVirtualGetPosition] = reinterpret_cast<void*>(CodeAddress(MockGetPosition));

    // The four calls the cutaway redirects. Each is the client's own
    // instruction at the client's own address, so it calls the client's
    // address for the routine, where a jump to the mock sits; around it is
    // just enough of a routine to make the call.
    at = game::kLocateCameraCall;
    Bytes(at, game::kLocateCameraCallBytes, sizeof(game::kLocateCameraCallBytes));
    Fixed<uint8_t>(at++) = 0xC3;  // ret
    JumpAt(kLocateCameraRoutine, CodeAddress(MockLocateCamera));

    at = game::kFinishVisitsCall;  // the two sit back to back, as in the client
    Bytes(at, game::kFinishVisitsCallBytes, sizeof(game::kFinishVisitsCallBytes));
    Bytes(at, game::kBeforeDrawingCallBytes, sizeof(game::kBeforeDrawingCallBytes));
    Fixed<uint8_t>(at++) = 0xC3;
    JumpAt(kFinishVisitsRoutine, CodeAddress(MockFinishVisits));
    JumpAt(kBeforeDrawingRoutine, CodeAddress(MockBeforeDrawing));

    // The cursor's hit test: push end, start, y and x, `this` in ecx, call,
    // keep the result.
    at = kCursorHitTest;
    for (int i = 3; i >= 0; --i) {
        Fixed<uint8_t>(at++) = 0xFF;  // push dword [address]
        Fixed<uint8_t>(at++) = 0x35;
        Word(at, Address(&g_rayArguments[i]));
    }
    Fixed<uint8_t>(at++) = 0x8B;  // mov ecx, [address]
    Fixed<uint8_t>(at++) = 0x0D;
    Word(at, Address(&g_rayThis));
    Bytes(at, game::kCursorRayCallBytes, sizeof(game::kCursorRayCallBytes));  // lands on kCursorRayCall
    Fixed<uint8_t>(at++) = 0xA3;  // mov [address], eax
    Word(at, Address(&g_rayResult));
    Fixed<uint8_t>(at++) = 0xC3;
    JumpAt(kCursorRayRoutine, CodeAddress(MockCursorRay));

    JumpAt(game::kUpdateShaderProjection, CodeAddress(MockUpdateShaderProjection));
    JumpAt(game::kVisitBuildingGroup, CodeAddress(MockVisit));
    JumpAt(game::kBoxOutsideView, CodeAddress(MockBoxOutsideView));
    JumpAt(game::kIntersectWorld, CodeAddress(MockIntersect));
    JumpAt(game::kUnitIsOutdoors, CodeAddress(MockUnitIsOutdoors));

    // The graphics device, holding what the UI left on it: no perspective.
    *reinterpret_cast<void***>(g_device) = g_deviceVirtuals;
    g_deviceVirtuals[game::kGxDeviceVirtualSetProjection] = reinterpret_cast<void*>(CodeAddress(MockSetProjection));
    Fixed<uint8_t*>(game::kGxDevicePointer) = g_device;
    game::Matrix identity = {};
    for (int i = 0; i < 4; ++i) {
        identity.m[i][i] = 1.0f;
    }
    game::At<game::Matrix>(g_device, game::kGxDeviceProjection) = identity;
    game::At<game::Matrix>(g_device, game::kGxDeviceHardwareProjection) = identity;
    game::At<game::Matrix>(g_device, game::kGxDeviceViewStack) = identity;
    game::At<uint32_t>(g_device, game::kGxDeviceViewLevel) = 0;

    // The scene: whole-screen view, no buildings placed yet.
    Fixed<int32_t>(game::kSceneFrustumLevel) = 0;
    Fixed<uint32_t>(game::kSceneInteriorPath) = 0;
    Fixed<uintptr_t>(game::kPlacedBuildingsHead) = 0;
    Fixed<int32_t>(game::kPlacedBuildingsLinkOffset) = kBuildingListLinkOffset;

    // The world: in it, with a world frame whose active camera is g_camera.
    Fixed<uint8_t>(game::kInWorldFlag) = 1;
    Fixed<void*>(game::kSpellTargetingPointer) = nullptr;
    Fixed<uint8_t*>(game::kWorldFramePointer) = g_worldFrame;
    game::At<void*>(g_worldFrame, game::kWorldFrameCamera) = g_camera;
}

// ---------------------------------------------------------------------------
// Doing what the client does
// ---------------------------------------------------------------------------

// A camera in its normal state in the world: set up on its target, plus some
// flag the library has no business with.
constexpr uint32_t kCameraFlagUnrelated = 0x100;
constexpr uint32_t kClientFlags = game::kCameraFlagInitialized | kCameraFlagUnrelated;
constexpr float kClientZoom = 12.0f;
constexpr float kClientZoomBlend = 11.0f;

void ResetCamera(uint8_t* camera) {
    game::At<uint32_t>(camera, game::kCameraFlags) = kClientFlags;
    game::At<float>(camera, game::kCameraZoom) = kClientZoom;
    game::At<float>(camera, game::kCameraZoomBlend) = kClientZoomBlend;
}

uint32_t CameraFlags(uint8_t* camera) {
    return game::At<uint32_t>(camera, game::kCameraFlags);
}

game::lua_State* LuaState() {
    return reinterpret_cast<game::lua_State*>(&g_lua);
}

void LuaClear() {
    g_lua.top = 0;
    g_lua.pushes = 0;
    g_lua.pushedString = nullptr;
    g_lua.pushedNumberCount = 0;
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

// Calls a registered function with up to three numbers.
int CallLuaWith(const char* name, int count, double a = 0.0, double b = 0.0, double c = 0.0) {
    const double values[] = {a, b, c};
    LuaClear();
    for (int i = 0; i < count; ++i) {
        LuaPushNumber(values[i]);
    }
    return CallLua(name);
}

void SetCamera(double yaw, double pitch, double distance) {
    CallLuaWith("ARPG_SetCamera", 3, yaw, pitch, distance);
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
    CallLuaWith("ARPG_SetCamera", 0);
}

void SetHoldInterval(double milliseconds) {
    CallLuaWith("ARPG_SetHoldInterval", 1, milliseconds);
}

void Press() {
    CallLuaWith("ARPG_HoldStart", 0);
}

void Release() {
    CallLuaWith("ARPG_HoldStop", 0);
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

float Sine(float angle) {
    float result;
    __asm__("fsin" : "=t"(result) : "0"(angle));
    return result;
}

float Cosine(float angle) {
    float result;
    __asm__("fcos" : "=t"(result) : "0"(angle));
    return result;
}

// One camera update, step for step as the client's own routine does it
// (0x00606F90 in the 12340 executable): set the camera up if it is not yet,
// get the angles, get the distance, take the flag bits the distance routine
// returns, drop to first person if that bit is now set, place the camera
// behind the pivot along the matrix's forward axis, and note a collision if
// the camera ended up closer than its zoom.
constexpr uint32_t kCameraFlagsClearedPerUpdate = 0x30000;
constexpr uint32_t kCameraFlagKeepDistance = 0x40000000;
constexpr uint32_t kCameraFlagCollisionBlend = 0x4000000;
constexpr float kCollisionBlendThreshold = 0.111111f;
constexpr float kCollisionBlendMargin = 0.111112f;
constexpr float kFirstPersonZoom = 1.8315f;  // below this zoom the client treats the view as first person

void ClientCameraUpdate(uint8_t* camera, void* target) {
    uint32_t& flags = game::At<uint32_t>(camera, game::kCameraFlags);
    flags |= game::kCameraFlagInitialized;

    float yaw = 0.0f;
    float pitch = 0.0f;
    float roll = 0.0f;
    int popped = 0;
    const uint32_t angleArguments[] = {Address(target), Address(&yaw), Address(&pitch), Address(&roll)};
    CallClient(game::kCameraOrientation, camera, angleArguments, 4, &popped);

    flags &= ~kCameraFlagsClearedPerUpdate;
    float pivot[3] = {g_playerPosition.x, g_playerPosition.y, g_playerPosition.z};
    float offset[3] = {0.0f, 0.0f, 0.0f};
    float distance = 0.0f;
    float height = 0.0f;
    float fraction = 0.0f;
    const uint32_t distanceArguments[] = {Address(pivot),  Address(&distance),     Address(&height),
                                          Address(offset), Bits(kCollisionScale), Address(&fraction)};
    flags |= CallClient(game::kCameraCollision, camera, distanceArguments, 6, &popped);
    if ((flags & game::kCameraFlagFirstPerson) != 0 && (flags & kCameraFlagKeepDistance) == 0) {
        distance = 0.0f;
    }

    const float matrixYaw = game::At<float>(camera, kMockMatrixYaw);
    const float matrixPitch = game::At<float>(camera, kMockMatrixPitch);
    game::Vec3& position = game::At<game::Vec3>(camera, game::kCameraPosition);
    position.x = pivot[0] - Cosine(matrixYaw) * Cosine(matrixPitch) * distance;
    position.y = pivot[1] - Sine(matrixYaw) * Cosine(matrixPitch) * distance;
    position.z = pivot[2] + height + Sine(matrixPitch) * distance;

    float& zoom = game::At<float>(camera, game::kCameraZoom);
    if (zoom - distance > kCollisionBlendThreshold) {
        flags |= kCameraFlagCollisionBlend;
        zoom = distance + kCollisionBlendMargin;
    }
}

bool ClientSeesFirstPerson(uint8_t* camera) {
    return game::At<float>(camera, game::kCameraZoom) < kFirstPersonZoom;
}

// Where a camera at these angles and this distance from the character is.
game::Vec3 CameraSpot(float yawDegrees, float pitchDegrees, float distance) {
    const float yaw = yawDegrees * 0.017453292f;
    const float pitch = pitchDegrees * 0.017453292f;
    return {g_playerPosition.x - Cosine(yaw) * Cosine(pitch) * distance,
            g_playerPosition.y - Sine(yaw) * Cosine(pitch) * distance,
            g_playerPosition.z + kMockHeight + Sine(pitch) * distance};
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

// The cursor's ray, as seen from a camera high up to the south looking down
// to the north at 45 degrees. The character stands at (100, 200, 10); the
// ray comes down to the height of its feet at (110, 200).
constexpr game::Vec3 kRayStart = {70.0f, 200.0f, 50.0f};
constexpr game::Vec3 kRayEnd = {170.0f, 200.0f, -50.0f};
constexpr game::Vec3 kGroundUnderCursor = {110.0f, 200.0f, 10.0f};
constexpr game::Vec3 kRoofUnderCursor = {90.0f, 200.0f, 30.0f};   // 20 above the feet, on the same ray
constexpr game::Vec3 kStepUnderCursor = {108.0f, 200.0f, 12.0f};  // 2 above the feet, on the same ray

void PointCursorAt(int kind, const game::Vec3& hit) {
    g_cursor.kind = kind;
    g_cursor.hit = hit;
    g_cursor.rayStart = kRayStart;
    g_cursor.rayEnd = kRayEnd;
    g_cursor.guidLow = 0;
    g_cursor.guidHigh = 0;
}

void CheckPosition(const game::Vec3& actual, const game::Vec3& expected, const char* what) {
    float worst = 0.0f;
    const float differences[] = {actual.x - expected.x, actual.y - expected.y, actual.z - expected.z};
    for (float difference : differences) {
        const float size = difference < 0.0f ? -difference : difference;
        worst = size > worst ? size : worst;
    }
    Check(worst < 0.01f, what);
    if (worst >= 0.01f) {
        Print("          got (");
        PrintNumber(Thousandths(actual.x));
        Print(", ");
        PrintNumber(Thousandths(actual.y));
        Print(", ");
        PrintNumber(Thousandths(actual.z));
        Print(")/1000\n");
    }
}

// ---------------------------------------------------------------------------
// The checks
// ---------------------------------------------------------------------------

constexpr uint8_t kJumpOpcode = 0xE9;
constexpr float kRadiansPerDegree = 0.017453292f;
constexpr int kLuaFunctionCount = 11;

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
    CheckEqual(g_registeredCount, kLuaFunctionCount, "the library registers its Lua functions");

    const char* const names[] = {"ARPG_ClientVersion", "ARPG_HoldStart",       "ARPG_HoldStop",
                                 "ARPG_SetHoldInterval", "ARPG_SetGroundClicks", "ARPG_MoveTo",
                                 "ARPG_PlayerPosition", "ARPG_SetCamera",       "ARPG_CameraInfo",
                                 "ARPG_SetCutaway",     "ARPG_CutawayInfo"};
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
    CheckEqual(CallLuaWith("ARPG_ClientVersion", 0), 2, "returns two values");
    CheckEqual(g_lua.pushes, 2, "pushed as two values");
    const bool hasVersion = g_lua.pushedString != nullptr && g_lua.pushedString[0] != '\0';
    Check(hasVersion, "a version string");
    if (hasVersion) {
        Print("          version ");
        Print(g_lua.pushedString);
        Print("\n");
    }
    CheckEqual(static_cast<int>(g_lua.pushedNumbers[0]), 31, "and the feature bits: all five parts working");
}

void TestPlayerPosition() {
    Section("ARPG_PlayerPosition");
    CheckEqual(CallLuaWith("ARPG_PlayerPosition", 0), 3, "returns three values");
    CheckEqual(static_cast<int>(g_lookupTypeMask), static_cast<int>(game::kTypeMaskPlayer),
               "after asking the client for a player object");
    const game::Vec3 reported = {static_cast<float>(g_lua.pushedNumbers[0]), static_cast<float>(g_lua.pushedNumbers[1]),
                                 static_cast<float>(g_lua.pushedNumbers[2])};
    CheckPosition(reported, g_playerPosition, "the character's position");

    g_playerGuid = 0;
    CheckEqual(CallLuaWith("ARPG_PlayerPosition", 0), 0, "nothing when there is no character");
    g_playerGuid = kMockPlayerGuid;
    Fixed<uint8_t>(game::kInWorldFlag) = 0;
    CheckEqual(CallLuaWith("ARPG_PlayerPosition", 0), 0, "nothing outside the world");
    Fixed<uint8_t>(game::kInWorldFlag) = 1;
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
    CheckEqual(static_cast<int>(distance.returned), static_cast<int>(kCollisionResult), "its return value comes back whole");
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
    Check((g_collision.flags & game::kCameraFlagInitialized) != 0,
          "the lock holds on a camera in its normal, set-up state");
    CheckNear(g_collision.zoom, 30.0f, "the distance routine sees the locked distance as the zoom");
    CheckNear(g_collision.zoomBlend, 30.0f, "in both zoom fields");
    Check((g_collision.flags & game::kCameraFlagFirstPerson) != 0, "and is told not to test for collisions");
    Check((g_collision.flags & kCameraFlagUnrelated) != 0, "with the camera's other flags left alone");
    CheckNear(distance.distance, 30.0f, "the camera sits at the locked distance, through the obstacle");
    CheckEqual(static_cast<int>(distance.returned), static_cast<int>(kCollisionResult & ~game::kCameraFlagFirstPerson),
               "the return value comes back without the first-person bit");
    CheckEqual(distance.popped, 24, "the stack is as the client expects");
    CheckEqual(static_cast<int>(CameraFlags(g_camera)), static_cast<int>(kClientFlags),
               "afterwards the camera's flags are what they were");
    CheckNear(game::At<float>(g_camera, game::kCameraZoom), 30.0f,
              "and the camera's own zoom stays at the locked distance, for the rest of the client to see");

    game::At<uint32_t>(g_camera, game::kCameraFlags) |= game::kCameraFlagFirstPerson;
    distance = Collision(g_camera);
    CheckNear(distance.distance, 30.0f, "a camera that was in first person is locked all the same");
    CheckEqual(static_cast<int>(CameraFlags(g_camera)), static_cast<int>(kClientFlags), "and leaves first person");

    Section("camera, locked with collision: ARPG_SetCamera(90, 55, 30, true)");
    SetCameraColliding(90.0, 55.0, 30.0);
    angles = Orientation(g_camera, &g_followed);
    CheckNear(angles.yaw, 90.0f * kRadiansPerDegree, "yaw is given in degrees");
    game::At<uint32_t>(g_camera, game::kCameraFlags) |= game::kCameraFlagFirstPerson;
    distance = Collision(g_camera);
    Check((g_collision.flags & game::kCameraFlagFirstPerson) == 0, "the distance routine tests for collisions");
    CheckNear(g_collision.zoom, 30.0f, "starting from the locked distance");
    CheckNear(distance.distance, kObstacleDistance, "so the camera stops at the obstacle");
    CheckEqual(static_cast<int>(distance.returned), static_cast<int>(kCollisionResult),
               "and what the routine returns reaches the client whole");

    Section("camera, limits: ARPG_SetCamera(0, 120, 500)");
    ResetCamera(g_camera);
    SetCamera(0.0, 120.0, 500.0);
    angles = Orientation(g_camera, &g_followed);
    CheckNear(angles.pitch, 89.0f * kRadiansPerDegree, "pitch stops short of straight down");
    distance = Collision(g_camera);
    CheckNear(distance.distance, 200.0f, "distance stops at 200");

    Section("camera, locked, but not ours to move");
    SetCamera(0.0, 55.0, 30.0);
    Collision(g_camera);
    angles = Orientation(g_otherCamera, &g_followed);
    CheckNear(angles.yaw, kMockYaw, "a camera that is not the active one keeps its angles");
    distance = Collision(g_otherCamera);
    CheckNear(distance.distance, kObstacleDistance, "and its distance");
    CheckNear(game::At<float>(g_otherCamera, game::kCameraZoom), kClientZoom, "and its zoom");
    angles = Orientation(g_camera, nullptr);
    CheckNear(angles.yaw, kMockYaw, "a camera that follows nothing keeps its angles");

    Section("ARPG_CameraInfo");
    CheckEqual(CallLuaWith("ARPG_CameraInfo", 0), 5, "returns five values");
    CheckEqual(static_cast<int>(g_lua.pushedNumbers[0]), 1, "a lock is asked for");
    const int angleCalls = static_cast<int>(g_lua.pushedNumbers[1]);
    const int angleLocked = static_cast<int>(g_lua.pushedNumbers[2]);
    const int distanceCalls = static_cast<int>(g_lua.pushedNumbers[3]);
    const int distanceLocked = static_cast<int>(g_lua.pushedNumbers[4]);
    CheckEqual(angleCalls, g_orientation.count, "it has seen every run of the angle routine");
    CheckEqual(distanceCalls, g_collision.count, "and of the distance routine");
    Check(angleLocked > 0 && angleLocked < angleCalls, "and counted the runs in which it replaced the angles");
    Check(distanceLocked > 0 && distanceLocked < distanceCalls, "and the distance");

    Section("camera, released: ARPG_SetCamera()");
    ReleaseCamera();
    g_matrix = {};
    angles = Orientation(g_camera, &g_followed);
    CheckNear(angles.yaw, kMockYaw, "the client's yaw is back");
    CheckNear(angles.pitch, kMockPitch, "and its pitch");
    CheckEqual(g_matrix.count, 0, "the matrix is not rebuilt");
    distance = Collision(g_camera);
    CheckNear(g_collision.zoom, kClientZoom, "the camera has the zoom it had before the lock");
    CheckNear(distance.distance, kObstacleDistance, "and collides again");
    CheckEqual(CallLuaWith("ARPG_CameraInfo", 0), 5, "ARPG_CameraInfo still answers");
    CheckEqual(static_cast<int>(g_lua.pushedNumbers[0]), 0, "no lock is asked for any more");
}

void TestCameraUpdates() {
    Section("whole camera updates, as the client runs them");
    g_collisionReturn = 0;  // the real routine returns no flag bits unless a collision pushes the camera all the way in
    ReleaseCamera();
    ResetCamera(g_camera);
    game::At<uint32_t>(g_camera, game::kCameraFlags) = kCameraFlagUnrelated;  // not set up yet: the first update does it
    ClientCameraUpdate(g_camera, &g_followed);
    const game::Vec3 clientSpot = {g_playerPosition.x - Cosine(kMockYaw) * Cosine(kMockPitch) * kObstacleDistance,
                                   g_playerPosition.y - Sine(kMockYaw) * Cosine(kMockPitch) * kObstacleDistance,
                                   g_playerPosition.z + kMockHeight + Sine(kMockPitch) * kObstacleDistance};
    CheckPosition(game::At<game::Vec3>(g_camera, game::kCameraPosition), clientSpot,
                  "not locked: the camera is where the client puts it, at the obstacle");

    ResetCamera(g_camera);
    SetCamera(0.0, 55.0, 30.0);
    for (int i = 0; i < 3; ++i) {
        ClientCameraUpdate(g_camera, &g_followed);
    }
    CheckPosition(game::At<game::Vec3>(g_camera, game::kCameraPosition), CameraSpot(0.0f, 55.0f, 30.0f),
                  "locked: after three updates the camera is 30 yards out, south of and above the character");
    CheckEqual(static_cast<int>(CameraFlags(g_camera)), static_cast<int>(kClientFlags),
               "its flags are those of a normal third-person camera: no first person, no collision noted");
    Check(!ClientSeesFirstPerson(g_camera), "and the client does not take the view for first person");

    SetCamera(90.0, 20.0, 60.0);
    ClientCameraUpdate(g_camera, &g_followed);
    CheckPosition(game::At<game::Vec3>(g_camera, game::kCameraPosition), CameraSpot(90.0f, 20.0f, 60.0f),
                  "new settings take hold on the next update: 60 yards out, east of the character, lower");

    ReleaseCamera();
    ClientCameraUpdate(g_camera, &g_followed);
    CheckNear(g_collision.zoom, kClientZoom, "released: the next update starts from the zoom the player had");

    // A player who had zoomed all the way in before the lock.
    game::At<float>(g_camera, game::kCameraZoom) = 0.0f;
    game::At<float>(g_camera, game::kCameraZoomBlend) = 0.0f;
    game::At<uint32_t>(g_camera, game::kCameraFlags) = kClientFlags | game::kCameraFlagFirstPerson;
    Check(ClientSeesFirstPerson(g_camera), "a camera zoomed all the way in is in first person");
    SetCamera(0.0, 55.0, 30.0);
    ClientCameraUpdate(g_camera, &g_followed);
    CheckPosition(game::At<game::Vec3>(g_camera, game::kCameraPosition), CameraSpot(0.0f, 55.0f, 30.0f),
                  "locked from first person: the camera still goes 30 yards out");
    Check(!ClientSeesFirstPerson(g_camera), "and the client shows the character again");
    ReleaseCamera();
    ClientCameraUpdate(g_camera, &g_followed);
    Check(ClientSeesFirstPerson(g_camera), "released: the player is back in first person");

    ResetCamera(g_camera);
    SetCameraColliding(0.0, 55.0, 30.0);
    for (int i = 0; i < 3; ++i) {
        ClientCameraUpdate(g_camera, &g_followed);
    }
    CheckPosition(game::At<game::Vec3>(g_camera, game::kCameraPosition), CameraSpot(0.0f, 55.0f, kObstacleDistance),
                  "locked with collision: the camera stays at the obstacle, update after update");
    CheckNear(g_collision.zoom, 30.0f, "each update starting again from the locked distance");

    ReleaseCamera();
    ResetCamera(g_camera);
    g_collisionReturn = kCollisionResult;
}

void TestHoldMove() {
    Section("frames with nothing held");
    g_frame = {};
    g_click = {};
    g_terrain = {};
    PointCursorAt(game::kHitGround, kStepUnderCursor);
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
    CheckPosition(g_click.performPosition, kStepUnderCursor, "at the spot the cursor is on");
    Frame();
    CheckEqual(g_click.captures, 1, "the frame after that does nothing: the repeat interval has not passed");

    Section("holding");
    SetHoldInterval(10.0);
    WaitForRepeat();
    Frame();
    CheckEqual(g_click.performs, 2, "once the interval has passed, the move is issued again");
    PointCursorAt(game::kHitObjectGeometry, kStepUnderCursor);
    WaitForRepeat();
    Frame();
    CheckEqual(g_click.performs, 3, "the deck of a ship under the cursor counts as ground");
    PointCursorAt(game::kHitObject, kStepUnderCursor);
    WaitForRepeat();
    Frame();
    CheckEqual(g_click.captures, 4, "over a unit the cursor is still followed");
    CheckEqual(g_click.performs, 3, "but a held button does not click the unit again and again");
    CheckEqual(g_terrain.count, 0, "and, without ground clicks, does not move either");
    PointCursorAt(game::kHitNothing, kStepUnderCursor);
    WaitForRepeat();
    Frame();
    CheckEqual(g_click.performs, 3, "and nothing under the cursor means nothing to do");

    Section("release: ARPG_HoldStop()");
    Release();
    PointCursorAt(game::kHitGround, kStepUnderCursor);
    WaitForRepeat();
    Frame();
    CheckEqual(g_click.captures, 5, "after the release nothing looks under the cursor any more");
    CheckEqual(g_click.performs, 3, "and nothing moves");

    Section("other presses");
    PointCursorAt(game::kHitObject, kStepUnderCursor);
    Press();
    Frame();
    CheckEqual(g_click.performs, 4, "a press on a unit acts on it once");
    CheckEqual(g_click.lastButton, game::kClickRight, "as a right click: interact, attack, loot");
    PointCursorAt(game::kHitGround, kStepUnderCursor);
    WaitForRepeat();
    Frame();
    CheckEqual(g_click.performs, 4, "and if the cursor then slips onto the ground, the character is not called off");
    Release();

    PointCursorAt(game::kHitNothing, kStepUnderCursor);
    Press();
    Frame();
    CheckEqual(g_click.performs, 4, "a press on nothing does nothing");
    Release();

    PointCursorAt(game::kHitGround, kStepUnderCursor);
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

// One press with the cursor as set up; returns the position the click acted on.
game::Vec3 ClickedPosition() {
    Press();
    Release();
    Frame();
    return g_click.performPosition;
}

void TestGroundClicks() {
    Section("ground clicks off: a click lands where the cursor's ray lands");
    g_click = {};
    g_terrain = {};
    PointCursorAt(game::kHitGround, kRoofUnderCursor);
    CheckPosition(ClickedPosition(), kRoofUnderCursor, "on the roof");

    Section("ground clicks: ARPG_SetGroundClicks(3)");
    CallLuaWith("ARPG_SetGroundClicks", 1, 3.0);
    PointCursorAt(game::kHitGround, kRoofUnderCursor);
    CheckPosition(ClickedPosition(), kGroundUnderCursor,
                  "a click on a roof goes to where the ray reaches the height of the character's feet");
    CheckEqual(g_click.lastButton, game::kClickRight, "still as a right click");
    PointCursorAt(game::kHitObjectGeometry, kRoofUnderCursor);
    CheckPosition(ClickedPosition(), kGroundUnderCursor, "so does a click high up on a game object's shape");
    PointCursorAt(game::kHitGround, kStepUnderCursor);
    CheckPosition(ClickedPosition(), kStepUnderCursor, "a click within reach of the feet stays where it is");
    const game::Vec3 below = {125.0f, 200.0f, -5.0f};
    PointCursorAt(game::kHitGround, below);
    CheckPosition(ClickedPosition(), below, "and so does a click below the character");

    PointCursorAt(game::kHitGround, kRoofUnderCursor);
    g_cursor.guidHigh = 0x1FC00000;
    CheckPosition(ClickedPosition(), kRoofUnderCursor, "a hit on a transport is left in the transport's coordinates");

    PointCursorAt(game::kHitGround, kRoofUnderCursor);
    g_cursor.rayStart = kRayEnd;
    g_cursor.rayEnd = kRayStart;
    CheckPosition(ClickedPosition(), kGroundUnderCursor, "it does not matter which end of the ray comes first");

    PointCursorAt(game::kHitGround, kRoofUnderCursor);
    g_cursor.rayEnd = {120.0f, 200.0f, 20.0f};
    CheckPosition(ClickedPosition(), kRoofUnderCursor, "a ray that never gets down to the character's height is left alone");

    PointCursorAt(game::kHitGround, kRoofUnderCursor);
    g_cursor.rayEnd = {10070.0f, 200.0f, -50.0f};
    CheckPosition(ClickedPosition(), kRoofUnderCursor, "and so is one that comes down thousands of yards away");

    PointCursorAt(game::kHitGround, kRoofUnderCursor);
    g_playerGuid = 0;
    CheckPosition(ClickedPosition(), kRoofUnderCursor, "and so is everything when the character cannot be found");
    g_playerGuid = kMockPlayerGuid;

    CallLuaWith("ARPG_SetGroundClicks", 1, 25.0);
    PointCursorAt(game::kHitGround, kRoofUnderCursor);
    CheckPosition(ClickedPosition(), kRoofUnderCursor, "ARPG_SetGroundClicks(25): the same roof now counts as within reach");
    CallLuaWith("ARPG_SetGroundClicks", 1, 3.0);

    Section("ground clicks: holding over a unit");
    SetHoldInterval(10.0);
    PointCursorAt(game::kHitGround, kStepUnderCursor);
    Press();
    Frame();
    const int performs = g_click.performs;
    PointCursorAt(game::kHitObject, kStepUnderCursor);
    WaitForRepeat();
    Frame();
    CheckEqual(g_click.performs, performs, "the unit is not clicked");
    CheckEqual(g_terrain.count, 1, "the character keeps walking instead");
    CheckPosition(g_terrain.last.position, kGroundUnderCursor, "towards the ground under the cursor");
    Check(g_terrain.last.button == game::kClickRight && g_terrain.last.transportGuidLow == 0 &&
              g_terrain.last.transportGuidHigh == 0,
          "as a right click on plain ground");
    Release();

    Section("ground clicks off again: ARPG_SetGroundClicks()");
    CallLuaWith("ARPG_SetGroundClicks", 0);
    PointCursorAt(game::kHitGround, kRoofUnderCursor);
    CheckPosition(ClickedPosition(), kRoofUnderCursor, "the click is back on the roof");
}

void TestMoveTo() {
    Section("ARPG_MoveTo(500, 600)");
    g_terrain = {};
    const int frames = g_frame.count;
    CallLuaWith("ARPG_MoveTo", 2, 500.0, 600.0);
    CheckEqual(g_terrain.count, 0, "the Lua call itself does nothing in the world");
    Frame();
    CheckEqual(g_frame.count, frames + 1, "the frame handler still runs");
    CheckEqual(g_terrain.count, 1, "the next frame clicks the ground once");
    const game::Vec3 expected = {500.0f, 600.0f, g_playerPosition.z};
    CheckPosition(g_terrain.last.position, expected, "at that spot, at the character's own height");
    Check(g_terrain.last.button == game::kClickRight && g_terrain.last.transportGuidLow == 0 &&
              g_terrain.last.transportGuidHigh == 0,
          "as a right click on plain ground");
    Frame();
    CheckEqual(g_terrain.count, 1, "and only once");

    CallLuaWith("ARPG_MoveTo", 3, 1.0, 2.0, 3.0);
    Frame();
    const game::Vec3 exact = {1.0f, 2.0f, 3.0f};
    CheckPosition(g_terrain.last.position, exact, "ARPG_MoveTo(1, 2, 3): a given height is used as it is");

    CallLuaWith("ARPG_MoveTo", 1, 5.0);
    Frame();
    CheckEqual(g_terrain.count, 2, "ARPG_MoveTo(5): without both coordinates nothing happens");

    CallLuaWith("ARPG_MoveTo", 2, 7.0, 8.0);
    Fixed<uint8_t>(game::kInWorldFlag) = 0;
    Frame();
    Fixed<uint8_t>(game::kInWorldFlag) = 1;
    Frame();
    CheckEqual(g_terrain.count, 2, "a request made outside the world is dropped, not kept for later");
}

void TestUiReload() {
    Section("UI reload");
    SetCamera(0.0, 55.0, 30.0);
    CallLuaWith("ARPG_SetGroundClicks", 1, 3.0);
    CallLuaWith("ARPG_MoveTo", 2, 9.0, 9.0);
    PointCursorAt(game::kHitGround, kRoofUnderCursor);
    Press();
    const int captures = g_click.captures;
    const int terrainClicks = g_terrain.count;
    const int uiLoads = g_uiLoads;
    const int registered = g_registeredCount;

    int popped = -1;
    CallClient(kRegistrationRoutine, nullptr, nullptr, 0, &popped);
    CheckEqual(g_uiLoads, uiLoads + 1, "the client registers its functions again");
    CheckEqual(g_registeredCount, registered + kLuaFunctionCount, "and so does the library");

    const Angles angles = Orientation(g_camera, &g_followed);
    CheckNear(angles.yaw, kMockYaw, "the camera lock is dropped until the new UI asks for it");
    WaitForRepeat();
    Frame();
    CheckEqual(g_click.captures, captures, "and so is the held button");
    CheckEqual(g_terrain.count, terrainClicks, "and a move that was still waiting");
    CheckPosition(ClickedPosition(), kRoofUnderCursor, "and ground clicks");
    CheckEqual(CallLuaWith("ARPG_CameraInfo", 0), 5, "ARPG_CameraInfo answers");
    Check(static_cast<int>(g_lua.pushedNumbers[1]) <= 1, "with its counts started afresh");
}

// ---------------------------------------------------------------------------
// The world frame's part: a whole frame of the 3D view, as the client runs it
// ---------------------------------------------------------------------------

float Tangent(float angle) {
    float result;
    __asm__("fptan\n\tfstp %%st(0)" : "=t"(result) : "0"(angle));
    return result;
}

float Magnitude(float value) {
    return value < 0.0f ? -value : value;
}

constexpr float kFieldOfView = 0.9424778f;  // the client's default 1.5708, times its 0.6
constexpr float kAspect = 16.0f / 9.0f;
constexpr float kNearDistance = 0.1f;
constexpr float kFarDistance = 357.0f;

// The projection the client builds for the world (0x006BF370).
game::Matrix WorldProjection() {
    const float t = Tangent(kFieldOfView * 0.5f);
    game::Matrix projection = {};
    projection.m[0][0] = 1.0f / (kAspect * t);
    projection.m[1][1] = 1.0f / t;
    projection.m[2][2] = (kFarDistance + kNearDistance) / (kFarDistance - kNearDistance);
    projection.m[2][3] = 1.0f;
    projection.m[3][2] = -2.0f * kFarDistance * kNearDistance / (kFarDistance - kNearDistance);
    return projection;
}

// A camera looking along `yaw` (0 = north, +x) and down by `pitch`: rows of
// the matrix are where the world's axes end up in view space, as right, up
// and depth.
game::Matrix ViewRotation(float yaw, float pitch) {
    const float right[3] = {Sine(yaw), -Cosine(yaw), 0.0f};
    const float up[3] = {Sine(pitch) * Cosine(yaw), Sine(pitch) * Sine(yaw), Cosine(pitch)};
    const float forward[3] = {Cosine(pitch) * Cosine(yaw), Cosine(pitch) * Sine(yaw), -Sine(pitch)};
    game::Matrix view = {};
    for (int i = 0; i < 3; ++i) {
        view.m[i][0] = right[i];
        view.m[i][1] = up[i];
        view.m[i][2] = forward[i];
    }
    view.m[3][3] = 1.0f;
    return view;
}

void DeviceSetProjection(const game::Matrix& projection) {
    // Through the virtual table, as the client calls it.
    const uint32_t arguments[] = {Address(&projection)};
    int popped = -1;
    CallClient(reinterpret_cast<uintptr_t>(g_deviceVirtuals[game::kGxDeviceVirtualSetProjection]), g_device,
               arguments, 1, &popped);
}

bool SameMatrix(const game::Matrix& a, const game::Matrix& b) {
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            if (Bits(a.m[i][j]) != Bits(b.m[i][j])) {
                return false;
            }
        }
    }
    return true;
}

// What one frame was drawn with.
struct DrawnFrame {
    game::Matrix hardware;  // the device's projection while the world was drawn
    game::Matrix shader;    // what the shaders had
    game::Matrix view;
    game::Vec3 eye;
    game::Matrix standard;  // what the hardware projection is without any cut
    int stackAfterLocate;
    int stackAfterStages;
    bool walkedFromCamera;  // the client drew from the building it found the camera in
    int setProjectionCalls; // between the scene update and the drawing
};
DrawnFrame g_drawn;
bool g_viewTransposed = false;  // a view matrix that does not mean what the library expects
float g_viewRoll = 0.0f;        // the camera tilted about its line of sight, which a vehicle can do to it
bool g_projectionFlat = false;  // a projection without perspective

void RenderWorldFrame() {
    // 0x004FAF90: remember the device's projection...
    const game::Matrix saved = game::At<game::Matrix>(g_device, game::kGxDeviceProjection);

    // ...update the camera, and set the device up from it (0x00607CB0).
    ClientCameraUpdate(g_camera, &g_followed);
    game::Matrix projection = WorldProjection();
    if (g_projectionFlat) {
        projection.m[2][3] = 0.0f;
        projection.m[3][3] = 1.0f;
    }
    DeviceSetProjection(projection);
    g_drawn.standard = game::At<game::Matrix>(g_device, game::kGxDeviceHardwareProjection);
    game::Matrix view = ViewRotation(game::At<float>(g_camera, kMockMatrixYaw), game::At<float>(g_camera, kMockMatrixPitch));
    if (g_viewRoll != 0.0f) {
        for (int i = 0; i < 3; ++i) {
            const float right = view.m[i][0];
            const float up = view.m[i][1];
            view.m[i][0] = right * Cosine(g_viewRoll) + up * Sine(g_viewRoll);
            view.m[i][1] = up * Cosine(g_viewRoll) - right * Sine(g_viewRoll);
        }
    }
    if (g_viewTransposed) {
        for (int i = 0; i < 3; ++i) {
            for (int j = i + 1; j < 3; ++j) {
                const float swap = view.m[i][j];
                view.m[i][j] = view.m[j][i];
                view.m[j][i] = swap;
            }
        }
    }
    game::At<game::Matrix>(g_device, game::kGxDeviceViewStack) = view;
    MockUpdateShaderProjection();

    // The scene update (0x007831A0): where the camera is, and in what.
    const game::Vec3 eye = game::At<game::Vec3>(g_camera, game::kCameraPosition);
    Fixed<game::Vec3>(game::kSceneCameraPosition) = eye;
    g_visitCount = 0;
    const int callsBefore = g_deviceCalls.setProjection;
    CallClient(game::kLocateCameraCall, nullptr, nullptr, 0, &g_drawn.stackAfterLocate);

    // The scene draw (0x0079A870): from the building the camera is in, or
    // from outdoors; then the two routines that close the search.
    g_drawn.walkedFromCamera = Fixed<void*>(game::kSceneCameraBuilding) != nullptr;
    CallClient(game::kFinishVisitsCall, nullptr, nullptr, 0, &g_drawn.stackAfterStages);

    // Drawing happens here, with whatever the device holds now.
    g_drawn.setProjectionCalls = g_deviceCalls.setProjection - callsBefore;
    g_drawn.hardware = game::At<game::Matrix>(g_device, game::kGxDeviceHardwareProjection);
    g_drawn.shader = g_deviceCalls.shaderProjection;
    g_drawn.view = view;
    g_drawn.eye = eye;

    // ...and put the device back.
    DeviceSetProjection(saved);
    MockUpdateShaderProjection();
}

struct ClipPoint {
    float x;
    float y;
    float z;
    float w;
};

ClipPoint Project(const game::Matrix& projection, const game::Vec3& point) {
    const float offset[3] = {point.x - g_drawn.eye.x, point.y - g_drawn.eye.y, point.z - g_drawn.eye.z};
    float view[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    for (int j = 0; j < 3; ++j) {
        view[j] = offset[0] * g_drawn.view.m[0][j] + offset[1] * g_drawn.view.m[1][j] + offset[2] * g_drawn.view.m[2][j];
    }
    float clip[4];
    for (int j = 0; j < 4; ++j) {
        clip[j] = view[0] * projection.m[0][j] + view[1] * projection.m[1][j] + view[2] * projection.m[2][j] +
                  view[3] * projection.m[3][j];
    }
    return {clip[0], clip[1], clip[2], clip[3]};
}

// Direct3D draws what lies between depth 0 and depth w.
bool Drawn(const game::Vec3& point) {
    const ClipPoint clip = Project(g_drawn.shader, point);
    return clip.w > 0.0f && clip.z >= 0.0f && clip.z <= clip.w;
}

// Whether the point is at the same place on screen as without a cut.
bool SameSpotOnScreen(const game::Vec3& point) {
    const ClipPoint cut = Project(g_drawn.shader, point);
    const ClipPoint plain = Project(g_drawn.standard, point);
    return Magnitude(cut.x - plain.x) < 0.0001f && Magnitude(cut.y - plain.y) < 0.0001f &&
           Magnitude(cut.w - plain.w) < 0.0001f;
}

bool Finite(const game::Matrix& matrix) {
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            const float value = matrix.m[i][j];
            if (!(value == value) || Magnitude(value) > 1e30f) {
                return false;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Buildings for the cutaway to find
// ---------------------------------------------------------------------------

struct GroupSpec {
    uint32_t flags;
    bool loaded;
    game::Box box;
};

constexpr uint32_t kIndoor = 0x2000;  // the model file's own "indoor" bit; the client and the library go by the others
constexpr game::Box kAroundCharacter = {{90.0f, 190.0f, 9.0f}, {110.0f, 210.0f, 13.0f}};
constexpr game::Box kUpstairs = {{90.0f, 190.0f, 15.0f}, {110.0f, 210.0f, 20.0f}};
constexpr game::Box kFarAway = {{900.0f, 900.0f, 9.0f}, {910.0f, 910.0f, 13.0f}};
constexpr game::Box kWholeBuilding = {{80.0f, 180.0f, 5.0f}, {120.0f, 220.0f, 40.0f}};

void PlaceBuilding(MockBuilding& mock, const GroupSpec* specs, int count, bool inlineArray, bool groupsPlaced,
                   bool modelLoaded, const game::Box& bounds) {
    mock = {};
    game::At<uint32_t>(mock.building, game::kBuildingFlags) = groupsPlaced ? game::kBuildingFlagGroupsPlaced | 0x1 : 0x1;
    game::At<game::Box>(mock.building, game::kBuildingBounds) = bounds;
    game::At<uint8_t*>(mock.building, game::kBuildingModel) = mock.model;
    game::At<int32_t>(mock.building, game::kBuildingGroupLinkOffset) = kGroupListLinkOffset;
    game::At<const uint8_t*>(mock.model, game::kModelGroupInfo) = mock.groupInfo;
    game::At<uint32_t>(mock.model, game::kModelGroupCount) = static_cast<uint32_t>(count);
    game::At<uint32_t>(mock.model, game::kModelLoaded) = modelLoaded ? 1 : 0;

    uintptr_t next = kListEnd;
    for (int i = count - 1; i >= 0; --i) {
        game::At<uint32_t>(mock.groupInfo, static_cast<size_t>(i) * game::kModelGroupInfoSize) = specs[i].flags;
        game::At<void*>(mock.model, game::kModelGroups + static_cast<size_t>(i) * sizeof(void*)) = mock.groupData[i];
        game::At<uint32_t>(mock.groupData[i], game::kGroupDataLoadFlags) =
            specs[i].loaded ? game::kGroupDataLoaded | 0x4 : 0x4;
        game::At<game::Box>(mock.placed[i], game::kPlacedGroupBounds) = specs[i].box;
        game::At<uint32_t>(mock.placed[i], game::kPlacedGroupIndex) = static_cast<uint32_t>(i);
        game::At<void*>(mock.links[i], game::kGroupLinkGroup) = mock.placed[i];
        game::At<uintptr_t>(mock.links[i], kGroupListLinkOffset + sizeof(void*)) = next;
        next = reinterpret_cast<uintptr_t>(mock.links[i]);
        mock.array[i] = mock.placed[i];
    }
    game::At<uintptr_t>(mock.building, game::kBuildingGroupLinks) = next;
    if (inlineArray) {
        game::At<int32_t>(mock.building, game::kBuildingGroupArrayMode) = count;
        for (int i = 0; i < count && i < 4; ++i) {
            game::At<void*>(mock.building, game::kBuildingGroupArray + static_cast<size_t>(i) * sizeof(void*)) =
                mock.placed[i];
        }
    } else {
        game::At<int32_t>(mock.building, game::kBuildingGroupArrayMode) = -1;
        game::At<uint32_t>(mock.building, game::kBuildingGroupArrayCount) = static_cast<uint32_t>(count);
        game::At<void**>(mock.building, game::kBuildingGroupArrayData) = mock.array;
    }
}

// Four buildings around the character:
//   0  a house: outer shell, the room the character is in, a room upstairs,
//      a room still loading, a courtyard lit by the sky, a far wing out of
//      view, and a part the client always treats as outdoors;
//   1  a building whose groups are not placed yet;
//   2  a small one-room hut (the client keeps up to four groups in the
//      building itself);
//   3  a building whose model has not arrived.
void PlaceBuildings() {
    const GroupSpec house[] = {
        {game::kGroupFlagOutdoor, true, kWholeBuilding},
        {kIndoor, true, kAroundCharacter},
        {kIndoor, true, kUpstairs},
        {kIndoor, false, kAroundCharacter},
        {kIndoor | game::kGroupFlagOutdoorLit, true, kAroundCharacter},
        {kIndoor, true, kFarAway},
        {kIndoor | game::kGroupFlagAlwaysOutdoor, true, kAroundCharacter},
    };
    const GroupSpec room[] = {{kIndoor, true, kAroundCharacter}};
    PlaceBuilding(g_buildings[0], house, 7, false, true, true, kWholeBuilding);
    PlaceBuilding(g_buildings[1], room, 1, true, false, true, kWholeBuilding);
    PlaceBuilding(g_buildings[2], room, 1, true, true, true, kWholeBuilding);
    PlaceBuilding(g_buildings[3], room, 1, true, true, false, kWholeBuilding);
    for (int i = 0; i < kMockBuildings; ++i) {
        game::At<uintptr_t>(g_buildings[i].building, kBuildingListLinkOffset + sizeof(void*)) =
            i + 1 < kMockBuildings ? reinterpret_cast<uintptr_t>(g_buildings[i + 1].building) : kListEnd;
    }
    Fixed<uintptr_t>(game::kPlacedBuildingsHead) = reinterpret_cast<uintptr_t>(g_buildings[0].building);
}

bool Visited(int building, uint32_t index, uint32_t interiorPath) {
    const int count = g_visitCount < kMaxVisits ? g_visitCount : kMaxVisits;
    for (int i = 0; i < count; ++i) {
        if (g_visits[i].building == g_buildings[building].building && g_visits[i].index == index) {
            return g_visits[i].interiorPath == interiorPath;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// The cutaway
// ---------------------------------------------------------------------------

struct CutawayInfo {
    int results;
    bool asked;
    bool cutting;
    float height;
    int frames;
    int framesCut;
    int framesSkipped;
    int rooms;
    bool indoors;
    bool blocked;
    bool refused;
    float overhead;
    float between;
};

CutawayInfo Info() {
    CutawayInfo info = {};
    info.results = CallLuaWith("ARPG_CutawayInfo", 0);
    const double* n = g_lua.pushedNumbers;
    info.asked = n[0] != 0.0;
    info.cutting = n[1] != 0.0;
    info.height = static_cast<float>(n[2]);
    info.frames = static_cast<int>(n[3]);
    info.framesCut = static_cast<int>(n[4]);
    info.framesSkipped = static_cast<int>(n[5]);
    info.rooms = static_cast<int>(n[6]);
    info.indoors = n[7] != 0.0;
    info.blocked = n[8] != 0.0;
    info.refused = n[9] != 0.0;
    info.overhead = static_cast<float>(n[10]);
    info.between = static_cast<float>(n[11]);
    return info;
}

void SetCutaway(double height) {
    CallLuaWith("ARPG_SetCutaway", 1, height);
}

void SetCutawayOutdoorsToo(double height) {
    LuaClear();
    LuaPushNumber(height);
    LuaPushBoolean(true);
    CallLua("ARPG_SetCutaway");
}

void CutawayOff() {
    CallLuaWith("ARPG_SetCutaway", 0);
}

// Runs frames, a little apart in time, until the cut has stopped moving (or
// has gone). Gives up after about three seconds.
CutawayInfo SettleFrames() {
    CutawayInfo info = {};
    float last = -1.0f;
    for (int i = 0, same = 0; i < 150 && same < 3; ++i) {
        platform::Sleep(20);
        RenderWorldFrame();
        info = Info();
        const float height = info.cutting ? info.height : 0.0f;
        same = Magnitude(height - last) < 0.0005f ? same + 1 : 0;
        last = height;
    }
    return info;
}

// Runs frames for about this long: for the things that only count once they
// have lasted.
void FramesFor(uint32_t milliseconds) {
    for (uint32_t waited = 0; waited < milliseconds; waited += 20) {
        platform::Sleep(20);
        RenderWorldFrame();
    }
}

constexpr float kCutHeight = 4.0f;
constexpr game::Vec3 kFeet = {100.0f, 200.0f, 10.0f};
constexpr game::Vec3 kHead = {100.0f, 200.0f, 12.0f};
constexpr game::Vec3 kJustUnderCut = {100.0f, 200.0f, 13.9f};
constexpr game::Vec3 kJustOverCut = {100.0f, 200.0f, 14.2f};
constexpr game::Vec3 kRoofOverhead = {100.0f, 200.0f, 18.0f};
constexpr game::Vec3 kFloorNorth = {112.0f, 203.0f, 10.0f};
constexpr game::Vec3 kRoofNorth = {112.0f, 203.0f, 20.0f};
constexpr game::Vec3 kFloorSouth = {95.0f, 197.0f, 10.0f};
constexpr game::Vec3 kCellar = {101.0f, 200.0f, 2.0f};
constexpr game::Vec3 kFarGround = {300.0f, 200.0f, 10.0f};
constexpr game::Vec3 kJustUnderCutWest = {104.0f, 212.0f, 13.9f};
constexpr game::Vec3 kJustOverCutWest = {104.0f, 212.0f, 14.2f};
constexpr game::Vec3 kJustUnderCutEast = {104.0f, 188.0f, 13.9f};
constexpr game::Vec3 kJustOverCutEast = {104.0f, 188.0f, 14.2f};

void TestCutawayOff() {
    Section("cutaway: installed, and off until asked for");
    Check(Fixed<uint8_t>(game::kLocateCameraCall) == 0xE8 && Fixed<uint8_t>(game::kFinishVisitsCall) == 0xE8 &&
              Fixed<uint8_t>(game::kBeforeDrawingCall) == 0xE8 && Fixed<uint8_t>(game::kCursorRayCall) == 0xE8,
          "the four calls are still calls");
    Check(CallTarget(game::kLocateCameraCall, &Fixed<uint8_t>(game::kLocateCameraCall)) != kLocateCameraRoutine &&
              CallTarget(game::kFinishVisitsCall, &Fixed<uint8_t>(game::kFinishVisitsCall)) != kFinishVisitsRoutine &&
              CallTarget(game::kBeforeDrawingCall, &Fixed<uint8_t>(game::kBeforeDrawingCall)) != kBeforeDrawingRoutine &&
              CallTarget(game::kCursorRayCall, &Fixed<uint8_t>(game::kCursorRayCall)) != kCursorRayRoutine,
          "and each now goes to the library");

    g_collisionReturn = 0;
    ResetCamera(g_camera);
    SetCamera(0.0, 60.0, 22.0);
    PlaceBuildings();
    g_outdoors = false;  // the character stands in the house
    g_cameraStandsIn = g_buildings[0].building;
    g_locateCalls = 0;
    g_finishVisits = {};
    g_beforeDrawing = {};
    Fixed<uint32_t>(game::kSceneInteriorPath) = 7;
    RenderWorldFrame();
    RenderWorldFrame();
    CheckEqual(g_locateCalls, 2, "the client's routine still finds what the camera stands in, every frame");
    Check(g_drawn.walkedFromCamera, "and the client draws from there");
    CheckEqual(g_drawn.stackAfterLocate, 0, "the stack is as the client left it");
    Check(g_finishVisits.count == 2 && g_beforeDrawing.count == 2, "the two routines that close the search still run");
    CheckEqual(g_drawn.stackAfterStages, 0, "with the stack as the client left it");
    CheckEqual(g_visitCount, 0, "no room is queued that the client did not find itself");
    Check(SameMatrix(g_drawn.hardware, g_drawn.standard), "the world is drawn with the client's own projection");
    CheckEqual(g_drawn.setProjectionCalls, 0, "which nobody touched");
    Check(Drawn(kRoofOverhead), "so the roof over the character is drawn");

    const CutawayInfo info = Info();
    CheckEqual(info.results, 12, "ARPG_CutawayInfo returns twelve values");
    Check(!info.asked && !info.cutting && info.framesCut == 0, "nothing asked for, nothing cut");
    Check(info.frames >= 2, "though it has seen the frames go by");
}

void TestCutawayIndoors() {
    Section("cutaway indoors: ARPG_SetCutaway(4), camera 22 yards out at 60 degrees");
    g_locateCalls = 0;
    g_finishVisits = {};
    g_beforeDrawing = {};
    g_visitsOfUnreadyGroups = 0;
    SetCutaway(kCutHeight);
    RenderWorldFrame();
    CutawayInfo info = Info();
    Check(info.asked && info.cutting && info.indoors, "the cut starts on the first frame indoors");
    Check(info.height > kCutHeight + 1.0f, "from high up: it slides down, it does not snap");
    float last = info.height;
    bool onlyDown = true;
    bool neverTooLow = true;
    for (int i = 0; i < 6; ++i) {
        platform::Sleep(15);
        RenderWorldFrame();
        info = Info();
        onlyDown = onlyDown && info.height <= last + 0.0005f;
        neverTooLow = neverTooLow && info.height >= kCutHeight - 0.0005f;
        last = info.height;
    }
    Check(onlyDown && neverTooLow, "frame by frame it comes down, and never below the height asked for");
    info = SettleFrames();
    CheckNear(info.height, kCutHeight, "and comes to rest 4 yards over the character's feet");

    Check(g_outdoorsAskedOf == &g_player, "indoors or not is the client's own test, asked of the player's character");
    CheckEqual(g_locateCalls, 0, "the client's routine no longer looks for what the camera stands in");
    Check(Fixed<void*>(game::kSceneCameraBuilding) == nullptr && Fixed<void*>(game::kSceneCameraBuildingSecond) == nullptr &&
              Fixed<uint32_t>(game::kSceneCameraGroupCount) == 0 && Fixed<uint32_t>(game::kSceneCameraGroupCountSecond) == 0 &&
              Fixed<char>(game::kSceneCameraBuildingName) == '\0' && Fixed<char>(game::kSceneCameraGroupName) == '\0',
          "the scene is told the camera stands in no building, the way the client's routine tells it");
    Check(!g_drawn.walkedFromCamera, "so the client draws the outdoor world");
    CheckEqual(g_drawn.stackAfterLocate, 0, "the stack is as the client left it");

    Section("cutaway indoors: the rooms");
    CheckEqual(g_visitCount, 3, "three rooms are queued for drawing");
    Check(Visited(0, 1, 1), "the room the character stands in, as a room reached indoors");
    Check(Visited(0, 4, 0), "the courtyard lit by the sky, as one that is not");
    Check(Visited(2, 0, 1), "the one-room hut next door");
    Check(!Visited(0, 0, 0) && !Visited(0, 0, 1), "not the house's outer shell: the outdoor pass has it");
    Check(!Visited(0, 2, 0) && !Visited(0, 2, 1), "not the room upstairs: all of it is above the cut");
    Check(!Visited(0, 5, 0) && !Visited(0, 5, 1), "not the wing that is out of view");
    Check(!Visited(0, 6, 0) && !Visited(0, 6, 1), "not the part the client always treats as outdoors");
    CheckEqual(g_visitsOfUnreadyGroups, 0,
               "and nothing the visit routine would crash on: not the room still loading, not a building "
               "without placed groups, not one without its model");
    CheckEqual(g_finishVisits.visitsSeen, 3, "they are queued before the client goes through the visited rooms");
    CheckEqual(static_cast<int>(Fixed<uint32_t>(game::kSceneInteriorPath)), 7,
               "the client's own marker for the kind of room is back as it was");
    CheckEqual(info.rooms, 3, "ARPG_CutawayInfo counts them");

    Fixed<int32_t>(game::kSceneFrustumLevel) = 1;
    RenderWorldFrame();
    CheckEqual(g_visitCount, 0, "with the scene's view narrowed to a doorway, no room is queued");
    Fixed<int32_t>(game::kSceneFrustumLevel) = 0;

    Section("cutaway indoors: the cut");
    RenderWorldFrame();
    Check(SameMatrix(g_finishVisits.projection, WorldProjection()) && SameMatrix(g_beforeDrawing.projection, WorldProjection()),
          "while the client works out what is in view, the projection is still its own");
    CheckEqual(g_drawn.setProjectionCalls, 1, "then it is replaced, once");
    Check(SameMatrix(g_drawn.shader, g_drawn.hardware), "and handed to the shaders");
    Check(!SameMatrix(g_drawn.hardware, g_drawn.standard), "it is not the client's own");
    Check(Drawn(kFeet) && Drawn(kHead), "the character is drawn, head to foot");
    Check(Drawn(kJustUnderCut), "a point just under the cut is drawn");
    Check(!Drawn(kJustOverCut), "a point just over it is not");
    Check(!Drawn(kRoofOverhead) && !Drawn(kRoofNorth), "the roof is gone, over the character and further on");
    Check(Drawn(kFloorNorth) && Drawn(kFloorSouth), "the floor is drawn, north and south of the character");
    Check(Drawn(kCellar), "and what lies below the floor");
    Check(Drawn(kFarGround), "ground 200 yards off is not lost at the far end");
    Check(SameSpotOnScreen(kFeet) && SameSpotOnScreen(kFloorNorth) && SameSpotOnScreen(kFloorSouth) &&
              SameSpotOnScreen(kCellar),
          "everything drawn is exactly where it was on screen");
    Check(SameMatrix(game::At<game::Matrix>(g_device, game::kGxDeviceProjection), [] {
              game::Matrix identity = {};
              for (int i = 0; i < 4; ++i) {
                  identity.m[i][i] = 1.0f;
              }
              return identity;
          }()),
          "after the frame the device has what the UI left on it: the client's own restore still works");
    info = Info();
    Check(info.framesCut > 0 && info.framesSkipped == 0, "ARPG_CutawayInfo counts the frames drawn cut, none skipped");

    const int callsBefore = g_deviceCalls.setProjection;
    int popped = -1;
    CallClient(game::kFinishVisitsCall, nullptr, nullptr, 0, &popped);
    CheckEqual(g_deviceCalls.setProjection, callsBefore, "a draw without a scene update before it is left alone");

    Section("cutaway indoors: every camera angle from 20 to 89 degrees");
    bool allFinite = true;
    bool allCut = true;
    bool allPlaced = true;
    int steps = 0;
    for (float pitch = 20.0f; pitch <= 89.0f; pitch += 0.25f) {
        SetCamera(0.0, pitch, 22.0);
        RenderWorldFrame();
        ++steps;
        allFinite = allFinite && Finite(g_drawn.hardware);
        allCut = allCut && g_drawn.setProjectionCalls == 1 && Drawn(kFeet) && Drawn(kJustUnderCut) && !Drawn(kJustOverCut) &&
                 !Drawn(kRoofOverhead);
        allPlaced = allPlaced && SameSpotOnScreen(kFeet) && SameSpotOnScreen(kHead);
    }
    Check(steps > 250, "(a few hundred angles)");
    Check(allFinite, "the projection never holds anything but ordinary numbers");
    Check(allCut, "the cut is where it should be at every one of them");
    Check(allPlaced, "and nothing moves on screen");
    SetCamera(30.0, 60.0, 22.0);
    RenderWorldFrame();
    Check(Drawn(kFeet) && !Drawn(kRoofOverhead) && SameSpotOnScreen(kFeet), "the same with the camera turned to one side");
    SetCamera(0.0, 60.0, 22.0);
    RenderWorldFrame();
    Check(Drawn(kJustUnderCutWest) && !Drawn(kJustOverCutWest) && Drawn(kJustUnderCutEast) && !Drawn(kJustOverCutEast),
          "the cut is level: the same height at both sides of the screen");

    Section("cutaway indoors: a camera tilted to one side");
    bool tiltedCut = true;
    bool tiltedPlaced = true;
    const float rolls[] = {0.35f, -0.35f, 1.2f};
    for (float roll : rolls) {
        g_viewRoll = roll;
        RenderWorldFrame();
        tiltedCut = tiltedCut && g_drawn.setProjectionCalls == 1 && Drawn(kFeet) && Drawn(kJustUnderCut) &&
                    !Drawn(kJustOverCut) && Drawn(kJustUnderCutWest) && !Drawn(kJustOverCutWest) &&
                    Drawn(kJustUnderCutEast) && !Drawn(kJustOverCutEast) && !Drawn(kRoofOverhead) &&
                    !Drawn(kRoofNorth) && Drawn(kFloorNorth) && Drawn(kFloorSouth) && Finite(g_drawn.hardware);
        tiltedPlaced = tiltedPlaced && SameSpotOnScreen(kFeet) && SameSpotOnScreen(kFloorNorth) &&
                       SameSpotOnScreen(kJustUnderCutWest);
    }
    g_viewRoll = 0.0f;
    Check(tiltedCut, "the cut stays level in the world, at the same height, whichever way the camera is tilted");
    Check(tiltedPlaced, "and nothing moves on screen");
    Check(Info().framesSkipped == 0, "no frame was skipped for it");
    RenderWorldFrame();
}

void TestCutawayCursor() {
    Section("cutaway: the cursor's ray");
    // From the camera down through the roof to the floor north of the character.
    g_rayStart = g_drawn.eye;
    g_rayEnd = {g_drawn.eye.x + (kFloorNorth.x - g_drawn.eye.x) * 3.0f, g_drawn.eye.y + (kFloorNorth.y - g_drawn.eye.y) * 3.0f,
                g_drawn.eye.z + (kFloorNorth.z - g_drawn.eye.z) * 3.0f};
    g_rayFound = true;
    g_rayCalls = {};
    game::Vec3 start = {};
    game::Vec3 end = {};
    g_rayArguments[0] = Bits(0.25f);
    g_rayArguments[1] = Bits(0.75f);
    g_rayArguments[2] = Address(&start);
    g_rayArguments[3] = Address(&end);
    g_rayThis = g_worldFrame;
    int popped = -1;
    CallClient(kCursorHitTest, nullptr, nullptr, 0, &popped);
    CheckEqual(g_rayCalls.count, 1, "the client's routine still builds the ray");
    Check(g_rayCalls.worldFrame == g_worldFrame && Bits(g_rayCalls.x) == Bits(0.25f) && Bits(g_rayCalls.y) == Bits(0.75f),
          "with the world frame and cursor position it was given");
    CheckEqual(static_cast<int>(g_rayResult), 1, "its result reaches the client, and the stack is whole");
    CheckNear(start.z, kFeet.z + kCutHeight, "the ray now starts at the cut");
    const float along = (g_rayStart.z - start.z) / (g_rayStart.z - g_rayEnd.z);
    const game::Vec3 onTheLine = {g_rayStart.x + (g_rayEnd.x - g_rayStart.x) * along,
                                  g_rayStart.y + (g_rayEnd.y - g_rayStart.y) * along, start.z};
    CheckPosition(start, onTheLine, "at the point where the client's ray comes through it");
    CheckPosition(end, g_rayEnd, "its far end is unchanged");

    g_rayStart = kHead;
    g_rayEnd = kCellar;
    CallClient(kCursorHitTest, nullptr, nullptr, 0, &popped);
    CheckPosition(start, kHead, "a ray that starts below the cut is left alone");

    g_rayStart = g_drawn.eye;
    g_rayEnd = kRoofOverhead;
    CallClient(kCursorHitTest, nullptr, nullptr, 0, &popped);
    CheckPosition(start, g_drawn.eye, "and so is one that ends above it");

    start = {1.0f, 2.0f, 3.0f};
    g_rayFound = false;
    g_rayEnd = kCellar;
    CallClient(kCursorHitTest, nullptr, nullptr, 0, &popped);
    CheckEqual(static_cast<int>(g_rayResult), 0, "a cursor that is not over the world: the client's 0 comes back");
    const game::Vec3 untouched = {1.0f, 2.0f, 3.0f};
    CheckPosition(start, untouched, "and nothing is written");
    g_rayFound = true;
}

void TestCutawayMoving() {
    Section("cutaway indoors: jumping, climbing, falling");
    CutawayInfo info = SettleFrames();
    CheckNear(info.height, kCutHeight, "at rest the cut is 4 yards over the feet");

    // A jump as the game has it: off the ground at 7.96 yards a second,
    // falling at 19.29 yards a second each second, so 1.64 yards up and back
    // on the ground after 0.83 seconds.
    float highestOverFloor = 0.0f;
    bool characterWhole = true;
    for (int frame = 1; frame <= 33; ++frame) {
        const float time = 0.025f * static_cast<float>(frame);
        const float lift = frame == 33 ? 0.0f : 7.955f * time - 0.5f * 19.29f * time * time;
        g_playerPosition.z = kFeet.z + lift;
        platform::Sleep(25);
        RenderWorldFrame();
        info = Info();
        const float overFloor = info.height + lift;
        highestOverFloor = overFloor > highestOverFloor ? overFloor : highestOverFloor;
        characterWhole = characterWhole && info.height >= 3.0f - 0.0005f;
    }
    Check(highestOverFloor < kCutHeight + 0.75f,
          "a jump lifts the cut by well under a yard: only as far as keeps it clear of the character");
    Check(characterWhole, "and it does stay 3 yards clear of the character's feet throughout");
    CheckNear(info.height, kCutHeight, "on landing it is back at 4 yards, at once");

    // Up a staircase at 5 yards a second.
    bool clear = true;
    for (int i = 1; i <= 20; ++i) {
        g_playerPosition.z = kFeet.z + 0.2f * static_cast<float>(i);
        platform::Sleep(40);
        RenderWorldFrame();
        info = Info();
        clear = clear && info.height >= 3.0f - 0.0005f && info.height <= kCutHeight + 0.0005f;
    }
    Check(clear, "up a staircase the cut keeps between 3 and 4 yards over the feet");
    FramesFor(1500);
    info = SettleFrames();
    CheckNear(info.height, kCutHeight, "and at the top it settles at 4 again");

    // Off a ledge.
    g_playerPosition.z = kFeet.z;
    info = SettleFrames();
    CheckNear(info.height, kCutHeight, "after a drop the cut follows the character down to 4 yards over the feet");
    Check(Drawn(kFeet) && !Drawn(kRoofOverhead), "with the character in view and the roof gone");

    Section("cutaway: a camera too low to look down on anything");
    SetCamera(0.0, 5.0, 6.0);
    info = SettleFrames();
    Check(!info.cutting, "at 5 degrees and 6 yards there is no cut");
    Check(SameMatrix(g_drawn.hardware, g_drawn.standard), "the world is drawn with the client's own projection");
    CheckEqual(g_visitCount, 0, "and no rooms are queued");
    SetCamera(0.0, 60.0, 22.0);
    info = SettleFrames();
    CheckNear(info.height, kCutHeight, "back up high, the cut comes back");

    Section("cutaway: walking out of the door");
    g_outdoors = true;
    g_cameraStandsIn = nullptr;
    RenderWorldFrame();
    info = Info();
    Check(info.cutting && !info.indoors, "the cut does not vanish on the spot");
    platform::Sleep(30);
    RenderWorldFrame();
    Check(Info().height > kCutHeight + 0.5f, "it slides back up");
    g_locateCalls = 0;
    info = SettleFrames();
    Check(!info.cutting, "and then it is gone");
    Check(g_locateCalls > 0, "the client's own routine looks for what the camera stands in again");
    Check(SameMatrix(g_drawn.hardware, g_drawn.standard) && g_visitCount == 0 && Drawn(kRoofOverhead),
          "and the world is drawn whole, with the client's own projection");
}

void TestCutawayOutdoors() {
    Section("cutaway outdoors: something between the camera and the character");
    g_outdoors = true;
    g_slabs[0] = {true, 20.0f, 21.0f, kEverywhere};  // an archway overhead, its underside 10 yards over the feet
    g_intersect = {};
    CutawayInfo info = SettleFrames();
    Check(!info.cutting, "with ARPG_SetCutaway(4) nothing is cut outdoors");
    CheckEqual(g_intersect.count, 0, "and the line of sight is not even tried");

    SetCutawayOutdoorsToo(kCutHeight);
    RenderWorldFrame();
    Check(!Info().cutting, "ARPG_SetCutaway(4, true): a block does not count on its first frame");
    Check(g_intersect.upward > 0 && g_intersect.downward > 0, "the line of sight is tried in both directions");
    CheckEqual(static_cast<int>(g_intersect.flags), static_cast<int>(game::kIntersectBuildings),
               "against buildings only: not terrain, not trees");
    CheckEqual(static_cast<int>(g_intersect.last), 0, "with the routine's last argument as the client passes it");
    FramesFor(300);
    info = SettleFrames();
    Check(info.cutting && info.blocked && !info.indoors, "once it has lasted, the cut is made");
    CheckNear(info.height, 10.0f - 0.3f, "just under the lowest thing in the way, not at the indoor height");
    Check(Drawn(kFeet) && Drawn(kRoofOverhead), "what is below the archway is all still there");
    const game::Vec3 archway = {100.0f, 200.0f, 20.5f};
    Check(!Drawn(archway), "the archway is not");
    CheckEqual(g_visitCount, 4, "rooms under the cut are queued here too");
    Check(Visited(0, 2, 1), "this time with the room upstairs, which is below a cut this high");
    g_intersectCountsFromFarEnd = true;
    FramesFor(1500);
    CheckNear(Info().height, 10.0f - 0.3f,
              "the same if the client's routine counts 'first' from the other end of the line");
    g_intersectCountsFromFarEnd = false;

    g_slabs[0].present = false;
    FramesFor(200);
    info = Info();
    Check(info.cutting, "the view clears: the cut stays a moment");
    CheckNear(info.height, 10.0f - 0.3f, "where it was");
    FramesFor(600);
    info = SettleFrames();
    Check(!info.cutting && !info.blocked, "and then slides away");

    // Something low: the cut never comes down onto the character.
    g_slabs[0] = {true, 12.0f, 12.5f, kEverywhere};
    FramesFor(300);
    info = SettleFrames();
    Check(info.cutting, "something low in the way, 2 yards up");
    CheckNear(info.height, 3.0f, "the cut stops 3 yards over the feet");
    g_slabs[0].present = false;
    FramesFor(600);
    SettleFrames();

    // A parapet: 1.2 yards high, right in front of the character.
    g_slabs[0] = {true, 10.2f, 11.2f, 99.8f};
    FramesFor(300);
    info = SettleFrames();
    Check(!info.cutting && !info.blocked, "a waist-high wall in front of the character hides nothing worth a cut");
    g_slabs[0].present = false;

    // A lamp post going by.
    g_slabs[0] = {true, 20.0f, 21.0f, kEverywhere};
    RenderWorldFrame();
    platform::Sleep(30);
    RenderWorldFrame();
    g_slabs[0].present = false;
    FramesFor(300);
    info = SettleFrames();
    Check(!info.cutting, "a block that lasts a few frames never starts a cut");

    // A routine that does not report hits the way this library reads them.
    g_slabs[0] = {true, 20.0f, 21.0f, kEverywhere};
    g_intersectLies = true;
    FramesFor(300);
    info = SettleFrames();
    Check(!info.cutting && !info.blocked, "a hit that is not where its own 'how far along' puts it is not believed");
    g_intersectLies = false;
    g_slabs[0].present = false;
    SettleFrames();
    SetCutaway(kCutHeight);
}

void TestCutawayTallRooms() {
    Section("cutaway indoors: ordinary rooms, tall rooms, great halls");
    g_outdoors = false;
    SetCutaway(kCutHeight);
    g_slabs[0] = {true, 15.0f, 16.0f, kEverywhere};
    CutawayInfo info = SettleFrames();
    CheckNear(info.height, kCutHeight, "a ceiling 5 yards up is an ordinary room: cut at the height asked for");

    CheckNear(info.overhead, 5.0f, "ARPG_CutawayInfo says how far overhead the building is");

    g_slabs[0] = {true, 22.0f, 23.0f, kEverywhere};
    FramesFor(2200);
    info = SettleFrames();
    CheckNear(info.height, 12.0f - 0.3f, "a ceiling 12 yards up is a tall room: cut just under the ceiling");
    const game::Vec3 highOnTheWall = {100.0f, 200.0f, 20.0f};
    const game::Vec3 inTheCeiling = {100.0f, 200.0f, 22.5f};
    Check(Drawn(highOnTheWall) && !Drawn(inTheCeiling), "the walls stay, the ceiling goes");

    // A gallery to the south, between the camera and the character.
    g_slabs[1] = {true, 18.0f, 18.5f, 97.0f};
    FramesFor(200);
    info = Info();
    CheckNear(info.height, 8.0f - 0.3f, "something lower gets in the way: the cut comes down under it at once");
    CheckNear(info.overhead, 12.0f, "ARPG_CutawayInfo still has the ceiling 12 yards overhead");
    CheckNear(info.between, 8.0f, "and now a part of the building 8 yards up between camera and character");
    g_slabs[1].present = false;
    FramesFor(200);
    CheckNear(Info().height, 8.0f - 0.3f, "it is out of the way for a moment: the cut does not bob back up");
    g_slabs[1].present = true;
    FramesFor(100);
    g_slabs[1].present = false;
    FramesFor(300);
    CheckNear(Info().height, 8.0f - 0.3f, "nor between one beam and the next");
    FramesFor(2200);
    info = SettleFrames();
    CheckNear(info.height, 12.0f - 0.3f, "once the way has been clear for a while it goes back up");

    g_slabs[1] = {true, 12.0f, 14.0f, 99.0f};  // a wall right in front of the character, 4 yards high
    FramesFor(300);
    info = SettleFrames();
    CheckNear(info.height, kCutHeight, "something low in the way: down to the height asked for, and no lower");
    g_slabs[1].present = false;

    Section("cutaway indoors: low ceilings");
    g_slabs[0] = {true, 13.5f, 14.5f, kEverywhere};
    FramesFor(100);
    info = SettleFrames();
    CheckNear(info.height, 3.5f - 0.3f, "a ceiling 3.5 yards up, lower than the cut asked for: cut just under it");
    const game::Vec3 inTheLowCeiling = {100.0f, 200.0f, 13.8f};
    const game::Vec3 underTheLowCeiling = {103.0f, 200.0f, 13.0f};
    Check(!Drawn(inTheLowCeiling) && Drawn(underTheLowCeiling) && Drawn(kHead),
          "so the ceiling does not lie over the room like a lid");
    g_slabs[0] = {true, 12.6f, 13.6f, kEverywhere};
    info = SettleFrames();
    CheckNear(info.height, 2.6f - 0.3f, "2.6 yards up: under that too, closer to the character than it would otherwise come");
    g_slabs[0] = {true, 12.1f, 13.1f, kEverywhere};
    info = SettleFrames();
    CheckNear(info.height, 2.0f, "but never closer than 2 yards");

    // Jumping under a ceiling 4.5 yards up.
    g_slabs[0] = {true, 14.5f, 15.5f, kEverywhere};
    FramesFor(1300);
    info = SettleFrames();
    CheckNear(info.height, kCutHeight, "under a ceiling a little over the cut asked for, the cut is as asked");
    float highest = 0.0f;
    for (int frame = 1; frame <= 33; ++frame) {
        const float time = 0.025f * static_cast<float>(frame);
        const float lift = frame == 33 ? 0.0f : 7.955f * time - 0.5f * 19.29f * time * time;
        g_playerPosition.z = kFeet.z + lift;
        platform::Sleep(25);
        RenderWorldFrame();
        const float overFloor = Info().height + lift;
        highest = overFloor > highest ? overFloor : highest;
    }
    Check(highest < 4.5f - 0.3f + 0.0005f, "a jump there never takes the cut up into the ceiling");
    CheckNear(Info().height, kCutHeight, "and it is back at 4 yards on landing");

    // A doorway: something low overhead for a moment.
    g_slabs[1] = {true, 13.5f, 14.0f, kEverywhere};
    FramesFor(100);
    CheckNear(Info().height, 3.5f - 0.3f, "through a low doorway the cut dips under the lintel at once");
    g_slabs[1].present = false;
    FramesFor(400);
    CheckNear(Info().height, 3.5f - 0.3f, "stays there for a moment");
    FramesFor(1000);
    info = SettleFrames();
    CheckNear(info.height, kCutHeight, "and then goes back up");

    SetCutaway(8.0);
    FramesFor(1300);
    info = SettleFrames();
    CheckNear(info.height, 4.5f - 0.3f, "a cut asked for at 8 yards under a ceiling at 4.5 is made under the ceiling");
    SetCutaway(kCutHeight);
    info = SettleFrames();
    CheckNear(info.height, kCutHeight, "(and back to 4)");

    Section("cutaway indoors: a hall higher than the camera");
    g_slabs[0] = {true, 60.0f, 61.0f, kEverywhere};
    g_cameraStandsIn = g_buildings[0].building;
    FramesFor(300);
    g_locateCalls = 0;
    info = SettleFrames();
    Check(!info.cutting && info.indoors, "a hall higher than the camera is left whole");
    Check(g_locateCalls > 0 && g_drawn.walkedFromCamera && SameMatrix(g_drawn.hardware, g_drawn.standard),
          "and drawn the client's own way, from the room the camera is in");

    g_slabs[1] = {true, 18.0f, 30.0f, 97.0f};  // a pillar's worth of masonry south of the character
    FramesFor(600);
    Check(!Info().cutting, "there, something between camera and character is left alone unless asked for");
    SetCutawayOutdoorsToo(kCutHeight);
    RenderWorldFrame();
    Check(!Info().cutting, "asked for, it still has to last a moment");
    FramesFor(300);
    info = SettleFrames();
    Check(info.cutting && info.blocked, "then it is cut");
    CheckNear(info.height, 8.0f - 0.3f, "just under the lowest of it");
    g_slabs[1].present = false;
    FramesFor(600);
    info = SettleFrames();
    Check(!info.cutting, "and the hall is whole again when it is out of the way");

    g_slabs[0].present = false;
    g_cameraStandsIn = nullptr;
    SetCutaway(kCutHeight);
    info = SettleFrames();
    CheckNear(info.height, kCutHeight, "indoors with no roof found at all: the look upwards is not believed, cut as asked");
}

void TestCutawayRefusals() {
    Section("cutaway: matrices that are not what the client's are");
    g_outdoors = false;
    CutawayInfo info = SettleFrames();
    CheckNear(info.height, kCutHeight, "indoors again, cut at 4 yards");
    const int skippedBefore = info.framesSkipped;

    g_viewTransposed = true;
    RenderWorldFrame();
    CheckEqual(g_drawn.setProjectionCalls, 0, "a view matrix that puts the character off screen: the projection is not touched");
    info = Info();
    CheckEqual(info.framesSkipped, skippedBefore + 1, "the frame is counted as skipped");
    g_viewTransposed = false;

    g_projectionFlat = true;
    RenderWorldFrame();
    CheckEqual(g_drawn.setProjectionCalls, 0, "a projection without perspective: not touched either");
    CheckEqual(Info().framesSkipped, skippedBefore + 2, "and counted");
    g_projectionFlat = false;

    uint8_t* const device = g_device;
    Fixed<uint8_t*>(game::kGxDevicePointer) = nullptr;
    RenderWorldFrame();
    CheckEqual(Info().framesSkipped, skippedBefore + 3, "no graphics device at all: skipped, not crashed");
    Fixed<uint8_t*>(game::kGxDevicePointer) = device;

    RenderWorldFrame();
    Check(g_drawn.setProjectionCalls == 1 && !Drawn(kRoofOverhead), "with the client's own matrices back, so is the cut");

    Section("cutaway: a renderer that treats the projection differently");
    g_deviceRescalesDepth = false;
    const int shaderUpdates = g_deviceCalls.shaderUpdates;
    RenderWorldFrame();
    CheckEqual(g_drawn.setProjectionCalls, 2, "the cut projection goes in, is found not to come out right, and is taken back");
    Check(SameMatrix(g_drawn.hardware, g_drawn.standard) && SameMatrix(g_drawn.shader, g_drawn.standard),
          "the world is drawn with the client's own projection, shaders included");
    Check(g_deviceCalls.shaderUpdates > shaderUpdates + 2, "(the shaders were told)");
    Check(!Info().refused, "one such frame is not yet the end of it");
    RenderWorldFrame();
    RenderWorldFrame();
    info = Info();
    Check(info.refused && !info.cutting, "after three the cutaway gives up");
    g_locateCalls = 0;
    RenderWorldFrame();
    Check(g_drawn.setProjectionCalls == 0 && g_locateCalls == 1 && g_visitCount == 0,
          "and leaves the frame entirely to the client");
    g_deviceRescalesDepth = true;
    RenderWorldFrame();
    Check(Info().refused, "it stays that way for the rest of this UI");

    Section("cutaway: off again, and outside the world");
    int popped = -1;
    CallClient(kRegistrationRoutine, nullptr, nullptr, 0, &popped);  // a UI reload
    info = Info();
    Check(!info.asked && !info.refused && info.frames == 0, "a new UI starts with the cutaway off and its counts at zero");
    RenderWorldFrame();
    Check(SameMatrix(g_drawn.hardware, g_drawn.standard), "nothing is cut until the new UI asks");
    SetCamera(0.0, 60.0, 22.0);
    SetCutaway(kCutHeight);
    info = SettleFrames();
    CheckNear(info.height, kCutHeight, "asked again, it cuts again");

    g_playerGuid = 0;
    RenderWorldFrame();
    Check(!Info().cutting && SameMatrix(g_drawn.hardware, g_drawn.standard), "no character: no cut");
    g_playerGuid = kMockPlayerGuid;
    Fixed<uint8_t>(game::kInWorldFlag) = 0;
    SettleFrames();
    RenderWorldFrame();
    Check(!Info().cutting && SameMatrix(g_drawn.hardware, g_drawn.standard), "outside the world: no cut");
    Fixed<uint8_t>(game::kInWorldFlag) = 1;

    SettleFrames();
    CutawayOff();
    RenderWorldFrame();
    const CutawayInfo off = Info();
    Check(!off.asked && !off.cutting && SameMatrix(g_drawn.hardware, g_drawn.standard) && g_visitCount == 0,
          "ARPG_SetCutaway(): off at once");

    SetCutaway(1.0);
    info = SettleFrames();
    CheckNear(info.height, 2.0f, "ARPG_SetCutaway(1): the lowest cut is 2 yards");
    SetCutaway(500.0);
    info = SettleFrames();
    Check(!info.cutting, "ARPG_SetCutaway(500): a cut above the camera is no cut");
    CutawayOff();
    ReleaseCamera();
    ResetCamera(g_camera);
    g_collisionReturn = kCollisionResult;
    g_outdoors = true;
    g_playerPosition = kFeet;
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
    TestPlayerPosition();
    TestCamera();
    TestCameraUpdates();
    TestHoldMove();
    TestGroundClicks();
    TestMoveTo();
    TestCutawayOff();
    TestCutawayIndoors();
    TestCutawayCursor();
    TestCutawayMoving();
    TestCutawayOutdoors();
    TestCutawayTallRooms();
    TestCutawayRefusals();
    TestUiReload();

    Print("\n");
    PrintNumber(g_checks);
    Print(" checks, ");
    PrintNumber(g_failures);
    Print(g_failures == 0 ? " failed: all passed\n" : " FAILED\n");
    return g_failures == 0 ? 0 : 1;
}
