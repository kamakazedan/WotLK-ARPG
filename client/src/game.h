// Client internals for World of Warcraft 3.3.5a, build 12340 (Wow.exe at its
// fixed image base 0x00400000).
//
// Every address and offset in this file was checked by disassembling the
// executable. Calling conventions are part of what was checked: a wrong one
// corrupts the stack, so do not change a signature without re-reading the code.
//
// test/mock_client.cpp builds a stand-in for the client from these same
// constants, so keep every address here as a named constant.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace game {

template <typename T>
inline T& At(void* base, size_t offset) {
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + offset);
}

// ---------------------------------------------------------------------------
// The executable itself
// ---------------------------------------------------------------------------

constexpr uintptr_t kImageBase = 0x00400000;
constexpr uint32_t kBuildTimestamp = 0x4C2452FE;  // PE header timestamp of build 12340

// ---------------------------------------------------------------------------
// Game state
// ---------------------------------------------------------------------------

// uint8_t: set by the client when the player enters the world, cleared when
// leaving.
constexpr uintptr_t kInWorldFlag = 0x00BD0792;

inline bool InWorld() {
    return *reinterpret_cast<const volatile uint8_t*>(kInWorldFlag) != 0;
}

// Pointer: non-null while a spell is waiting for the player to pick its
// target (the glowing hand cursor, or a reticle on the ground). The client's
// own SpellIsTargeting() tests the same pointer.
constexpr uintptr_t kSpellTargetingPointer = 0x00D3F4E4;

inline bool SpellIsTargeting() {
    return *reinterpret_cast<void* const volatile*>(kSpellTargetingPointer) != nullptr;
}

// ---------------------------------------------------------------------------
// Objects
// ---------------------------------------------------------------------------

struct Vec3 {
    float x;  // north
    float y;  // west
    float z;  // up
};

// uint64_t __cdecl (): the guid of the player's own character, 0 outside the
// world. Reads thread-local data: main thread only.
constexpr uintptr_t kPlayerGuid = 0x004D3790;

// void* __cdecl (guid low, guid high, type mask, file, line): the object with
// that guid if it is of one of the masked types, or null. The last two are
// the caller's source file name and line, for the client's own diagnostics.
constexpr uintptr_t kObjectByGuid = 0x004D4DB0;
constexpr uint32_t kTypeMaskPlayer = 0x10;

// Slot in every game object's virtual table:
// Vec3* __thiscall (Vec3* out), fills in the object's position and returns out.
constexpr size_t kObjectVirtualGetPosition = 11;

// The player's own character, or null. Main thread only.
inline void* PlayerObject() {
    const uint64_t guid = reinterpret_cast<uint64_t (__cdecl*)()>(kPlayerGuid)();
    if (guid == 0) {
        return nullptr;
    }
    return reinterpret_cast<void* (__cdecl*)(uint32_t, uint32_t, uint32_t, const char*, int)>(kObjectByGuid)(
        static_cast<uint32_t>(guid), static_cast<uint32_t>(guid >> 32), kTypeMaskPlayer, "WotLK-ARPG", 0);
}

inline void ObjectPosition(void* object, Vec3* out) {
    void** virtuals = *reinterpret_cast<void***>(object);
    reinterpret_cast<Vec3* (__thiscall*)(void*, Vec3*)>(virtuals[kObjectVirtualGetPosition])(object, out);
}

// ---------------------------------------------------------------------------
// World frame (the 3D view and everything clicked in it)
// ---------------------------------------------------------------------------

constexpr uintptr_t kWorldFramePointer = 0x00B7436C;  // CGWorldFrame*

inline uint8_t* WorldFrame() {
    return *reinterpret_cast<uint8_t* const volatile*>(kWorldFramePointer);
}

// What the last capture found under the cursor.
enum HitKind : int {
    kHitNothing = 0,
    kHitGround = 1,          // the static world: terrain, buildings, trees and other scenery
    kHitObject = 2,          // a unit or game object, to interact with
    kHitObjectGeometry = 3,  // the solid shape of a game object, such as a ship's deck; clicks treat it as ground
};

constexpr size_t kWorldFrameHitKind = 0x2D8;      // HitKind
constexpr size_t kWorldFrameHitGuid = 0x2E0;      // uint64_t: what was hit, where that has a guid
constexpr size_t kWorldFrameHitPosition = 0x2E8;  // Vec3: where the cursor's ray met it
constexpr size_t kWorldFrameRayStart = 0x2F8;     // Vec3: the cursor's ray, from the camera...
constexpr size_t kWorldFrameRayEnd = 0x304;       // Vec3: ...to the far end. Fresh whenever the kind is not nothing
constexpr size_t kWorldFrameCamera = 0x7E20;      // CGCamera*

// A hit on a transport (a ship, a lift) carries the transport's guid, and its
// position is then in the transport's own coordinates, not the world's. This
// is the client's own test for that case.
inline bool HitIsOnTransport(uint8_t* worldFrame) {
    const uint32_t guidHigh = At<uint32_t>(worldFrame, kWorldFrameHitGuid + 4);
    return (guidHigh & 0xF0000000u) == 0x10000000u && (guidHigh & 0x0FF00000u) == 0x0FC00000u;
}

// Mouse buttons as the click handlers number them.
constexpr int kClickLeft = 1;
constexpr int kClickRight = 4;

// void __thiscall CGWorldFrame(): hit-tests the world under the cursor and
// stores the result in the world frame. The client calls this on every
// mouse-button press.
constexpr uintptr_t kCaptureDefaultAction = 0x004FA570;

inline void CaptureDefaultAction(uint8_t* worldFrame) {
    reinterpret_cast<void (__thiscall*)(uint8_t*)>(kCaptureDefaultAction)(worldFrame);
}

// int __thiscall CGWorldFrame(int button): acts on the stored result as a
// click with the given button would. The client calls this when a mouse
// button is released without a drag.
//   Right: on terrain, click-to-move (if the autointeract CVar is on); on a
//          unit or object, interact, attack or loot.
//   Left:  on a unit, select it. If a spell is waiting for a target, give it
//          the unit or the spot on the ground instead.
constexpr uintptr_t kPerformDefaultAction = 0x004F7880;

inline int PerformDefaultAction(uint8_t* worldFrame, int button) {
    return reinterpret_cast<int (__thiscall*)(uint8_t*, int)>(kPerformDefaultAction)(worldFrame, button);
}

// int __cdecl (const TerrainClick*): what PerformDefaultAction ends in for a
// click on the ground. With the right button and the autointeract CVar on it
// starts click-to-move to the position; on foot the character has arrived
// when it is close in x and y, whatever z says.
struct TerrainClick {
    uint32_t transportGuidLow;   // zero unless the position is on a transport
    uint32_t transportGuidHigh;
    Vec3 position;
    int button;
};
constexpr uintptr_t kTerrainClick = 0x00527830;

inline void ClickTerrain(const TerrainClick& click) {
    reinterpret_cast<int (__cdecl*)(const TerrainClick*)>(kTerrainClick)(&click);
}

// ---------------------------------------------------------------------------
// Camera
// ---------------------------------------------------------------------------

inline void* ActiveCamera() {
    uint8_t* worldFrame = WorldFrame();
    return worldFrame != nullptr ? At<void*>(worldFrame, kWorldFrameCamera) : nullptr;
}

constexpr size_t kCameraPosition = 0x08;   // Vec3: where the camera is, written by every update
constexpr size_t kCameraFlags = 0x98;      // uint32_t
constexpr size_t kCameraZoom = 0x118;      // float: distance from the character the camera is zoomed to
constexpr size_t kCameraZoomBlend = 0x1E8; // float: zoom to return to once a collision has cleared

// Set once the camera has been set up on the object it follows, which is its
// normal state in the world. (Not a sign of anything special: an earlier
// version of this library read it as one and never locked the camera.)
constexpr uint32_t kCameraFlagInitialized = 0x4;

// First person. The distance routine returns this bit when a collision has
// pushed the camera all the way in, and its caller keeps it in the flags.
// While it is set the distance routine returns the zoom untested, and the
// caller then uses a distance of zero.
constexpr uint32_t kCameraFlagFirstPerson = 0x8;

// void __thiscall CGCamera(float yaw, float pitch, float roll): builds the
// camera's rotation matrix from angles in radians. Yaw 0 looks north (+X) and
// grows counter-clockwise; positive pitch looks down.
constexpr uintptr_t kBuildCameraMatrix = 0x00607BD0;

inline void BuildCameraMatrix(void* camera, float yaw, float pitch, float roll) {
    reinterpret_cast<void (__thiscall*)(void*, float, float, float)>(kBuildCameraMatrix)(camera, yaw, pitch, roll);
}

// ---------------------------------------------------------------------------
// The graphics device
//
// The engine keeps a view matrix and a projection matrix on the device.
// Matrices are 4x4 floats, row-major, used with row vectors: a point becomes
// p * view * projection. The projection the client builds for the world
// (0x006BF370) is
//     [ 1/(aspect*t)  0     0              0 ]
//     [ 0             1/t   0              0 ]      t = tan(fov / 2)
//     [ 0             0     (f+n)/(f-n)    1 ]      n, f = near and far distance
//     [ 0             0     -2fn/(f-n)     0 ]
// so view space has z pointing into the screen, clip-space w is that depth,
// and clip-space z runs from -w at the near distance to +w at the far one.
// ---------------------------------------------------------------------------

struct Matrix {
    float m[4][4];
};

constexpr uintptr_t kGxDevicePointer = 0x00C5DF88;  // CGxDevice*

inline uint8_t* GxDevice() {
    return *reinterpret_cast<uint8_t* const volatile*>(kGxDevicePointer);
}

constexpr size_t kGxDeviceProjection = 0x0F88;          // Matrix: as the engine set it
constexpr size_t kGxDeviceHardwareProjection = 0x0FC8;  // Matrix: what the Direct3D device made of it
constexpr size_t kGxDeviceViewLevel = 0x1AF8;           // uint32_t: top of the view matrix stack, 0 to 3
constexpr size_t kGxDeviceViewStack = 0x1B00;           // Matrix[4]

// Slot in the device's virtual table:
// void __thiscall (const Matrix*). The Direct3D device (0x006A9B40) keeps the
// matrix and derives the hardware one from it: depth is rescaled from -w..w
// to Direct3D's 0..w by replacing m[2][2] with (m[2][2] + 1) / 2 and m[3][2]
// with m[3][2] / 2 (it gets there by way of the near and far distances, and
// divides by m[2][2] - 1 on the way). No other element changes.
constexpr size_t kGxDeviceVirtualSetProjection = 40;

inline void SetProjection(uint8_t* device, const Matrix* projection) {
    void** virtuals = *reinterpret_cast<void***>(device);
    reinterpret_cast<void (__thiscall*)(uint8_t*, const Matrix*)>(virtuals[kGxDeviceVirtualSetProjection])(
        device, projection);
}

// void __cdecl (): hands the hardware projection to the vertex shaders. The
// client calls it after every change of projection while drawing.
constexpr uintptr_t kUpdateShaderProjection = 0x00872C10;

inline void UpdateShaderProjection() {
    reinterpret_cast<void (__cdecl*)()>(kUpdateShaderProjection)();
}

// ---------------------------------------------------------------------------
// The world scene
//
// Once per frame the world frame updates the camera, sets the device's view
// and projection from it (0x00607CB0), updates the scene (0x007831A0), draws
// it (0x0079A870) and puts the device's matrices back (all from 0x004FAF90).
//
// Buildings, caves and dungeons are "map objects": a placed building has
// groups (rooms and outer shells). Outdoor groups are drawn when they are in
// view. Indoor groups are drawn only when the camera stands in one of them,
// or sees into them through a doorway: the scene update finds the building
// the camera is in, and the draw walks from there, or from the outdoor groups
// in view, through the doorways. Every group reached is "visited", which
// queues it, the scenery inside it and the creatures inside it for drawing.
// ---------------------------------------------------------------------------

constexpr uintptr_t kSceneCameraPosition = 0x00CD8F5C;  // Vec3: the camera position the scene is drawn from

// What the scene update found the camera standing in.
constexpr uintptr_t kSceneCameraBuilding = 0x00CD87A4;         // placed building, or null outdoors
constexpr uintptr_t kSceneCameraBuildingSecond = 0x00CD87A0;   // a second one, where two overlap
constexpr uintptr_t kSceneCameraGroupCount = 0x00CDB0D8;       // uint32_t: how many of its groups
constexpr uintptr_t kSceneCameraGroupCountSecond = 0x00CDB0E8; // uint32_t
constexpr uintptr_t kSceneCameraBuildingName = 0x00CD8628;     // char[]: its file name, for the client's diagnostics
constexpr uintptr_t kSceneCameraGroupName = 0x00CD8730;        // char[]

constexpr uintptr_t kSceneFrustumLevel = 0x00CD8798;  // int32_t: 0 while the view is the whole screen
constexpr uintptr_t kSceneInteriorPath = 0x00CFBEB8;  // uint32_t: read by the visit routine, see below

// Every placed building, as an intrusive list: the head, and where in each
// building the link sits. A link's second word is the next building; the
// list ends at a null or odd value.
constexpr uintptr_t kPlacedBuildingsHead = 0x00D25440;
constexpr uintptr_t kPlacedBuildingsLinkOffset = 0x00D25438;  // int32_t

// A placed building.
constexpr size_t kBuildingFlags = 0x0C;       // uint32_t
constexpr size_t kBuildingBounds = 0x48;      // Box, world coordinates
constexpr size_t kBuildingModel = 0xF4;       // the building's model, shared by every placement of it
constexpr size_t kBuildingGroupLinkOffset = 0x114;  // int32_t: where in a group link its list link sits
constexpr size_t kBuildingGroupLinks = 0x11C; // first group link; same list rules as above
constexpr size_t kBuildingGroupArray = 0x120; // placed groups by index: four pointers here...
constexpr size_t kBuildingGroupArrayCount = 0x124;  // ...or, if the mode below is -1, a count...
constexpr size_t kBuildingGroupArrayData = 0x128;   // ...and a pointer to that many pointers
constexpr size_t kBuildingGroupArrayMode = 0x130;   // int32_t: how many of the four are used, or -1
constexpr uint32_t kBuildingFlagGroupsPlaced = 0x80;

constexpr size_t kGroupLinkGroup = 0x04;  // in a group link: the placed group

// A placed group.
constexpr size_t kPlacedGroupBounds = 0x24;  // Box, world coordinates
constexpr size_t kPlacedGroupIndex = 0x50;   // uint32_t: which of the model's groups

// A building's model.
constexpr size_t kModelGroupInfo = 0x130;   // array of 0x20-byte records, one per group, flags first
constexpr size_t kModelGroupCount = 0x16C;  // uint32_t
constexpr size_t kModelLoaded = 0x1E0;      // uint32_t: nonzero once the model file is in
constexpr size_t kModelGroups = 0x1F8;      // array of pointers to the groups' own data
constexpr size_t kModelGroupInfoSize = 0x20;

constexpr size_t kGroupDataLoadFlags = 0x198;  // uint32_t, in a group's own data
constexpr uint32_t kGroupDataLoaded = 0x1;

// Group flags, from the model file.
constexpr uint32_t kGroupFlagOutdoor = 0x8;             // drawn with the outdoor world
constexpr uint32_t kGroupFlagOutdoorLit = 0x40;         // indoors, but lit by the sky
constexpr uint32_t kGroupFlagAlwaysOutdoor = 0x10000;   // has no doorways; treated like an outdoor group

struct Box {
    Vec3 low;
    Vec3 high;
};

// void __cdecl (uint32_t groupIndex, void* building): the visit. Queues the
// placed group for drawing with the view currently on top of the scene's
// stack of views. The group must be placed and its data loaded: the routine
// does not check. It reads kSceneInteriorPath: nonzero marks the group as
// reached through indoor rooms only, which spares the creatures in it the
// test against the terrain's horizon.
constexpr uintptr_t kVisitBuildingGroup = 0x00799310;

inline void VisitBuildingGroup(uint32_t groupIndex, void* building) {
    reinterpret_cast<void (__cdecl*)(uint32_t, void*)>(kVisitBuildingGroup)(groupIndex, building);
}

// int __cdecl (const Box*): nonzero if the box lies wholly outside the view
// on top of the scene's stack.
constexpr uintptr_t kBoxOutsideView = 0x0078FB20;

inline bool BoxOutsideView(const Box* box) {
    return reinterpret_cast<int (__cdecl*)(const Box*)>(kBoxOutsideView)(box) != 0;
}

// bool __cdecl (const Vec3* from, const Vec3* to, Vec3* hit, float* fraction,
// uint32_t flags, uint32_t zero): whether the line meets the world, and where
// first. `fraction` goes in as 1 and comes back as how far along the line
// the hit is.
constexpr uintptr_t kIntersectWorld = 0x0077F310;

// What the line is tested against (0x007A3B70 decides from the flags):
//   0x10, 0x20, 0x40   the surfaces of buildings. 0x007AE140 turns the three
//                      into a mask of surface kinds: those that block
//                      movement, those that are drawn, and with 0x40 not
//                      those marked as letting the camera through.
//   0x100, and others  the terrain, by a second routine (0x007A39F0) that
//                      none of the three bits above brings in.
//   0x1, 0x100000, ... models and game objects (anything in 0x40F0000F).
// The client's own camera collision (0x00605D60) uses 0x00100171. Buildings
// alone, as the camera sees them:
constexpr uint32_t kIntersectBuildings = 0x00000070;

inline bool IntersectWorld(const Vec3& from, const Vec3& to, Vec3* hit, float* fraction, uint32_t flags) {
    *fraction = 1.0f;
    return reinterpret_cast<bool (__cdecl*)(const Vec3*, const Vec3*, Vec3*, float*, uint32_t, uint32_t)>(
        kIntersectWorld)(&from, &to, hit, fraction, flags, 0);
}

// bool __thiscall CGUnit(): whether the unit is outdoors; the client's
// IsIndoors() and IsOutdoors() are this test on the player (0x00612300).
constexpr uintptr_t kUnitIsOutdoors = 0x0071B7F0;

inline bool UnitIsOutdoors(void* unit) {
    return reinterpret_cast<bool (__thiscall*)(void*)>(kUnitIsOutdoors)(unit);
}

// ---------------------------------------------------------------------------
// Lua
// ---------------------------------------------------------------------------

struct lua_State;
using lua_CFunction = int (__cdecl*)(lua_State*);

constexpr int LUA_TNUMBER = 3;

// The client refuses to call C functions outside this address range. Both
// ends start at zero and are filled in with the bounds of Wow.exe's own code
// the first time a function is checked, unless they are already set.
constexpr uintptr_t kLuaFunctionRangeMin = 0x00D415B8;  // uint32_t
constexpr uintptr_t kLuaFunctionRangeMax = 0x00D415BC;  // uint32_t

// All __cdecl.
constexpr uintptr_t kRegisterLuaFunction = 0x00817F90;  // void (const char* name, lua_CFunction)
constexpr uintptr_t kLuaGetTop = 0x0084DBD0;            // int (lua_State*)
constexpr uintptr_t kLuaType = 0x0084DEB0;              // int (lua_State*, int index)
constexpr uintptr_t kLuaToNumber = 0x0084E030;          // double (lua_State*, int index)
constexpr uintptr_t kLuaToBoolean = 0x0084E0B0;         // int (lua_State*, int index)
constexpr uintptr_t kLuaPushNumber = 0x0084E2A0;        // void (lua_State*, double)
constexpr uintptr_t kLuaPushString = 0x0084E350;        // void (lua_State*, const char*)

// Registers a global function in the UI's Lua state.
inline void RegisterLuaFunction(const char* name, lua_CFunction function) {
    reinterpret_cast<void (__cdecl*)(const char*, lua_CFunction)>(kRegisterLuaFunction)(name, function);
}

inline int lua_gettop(lua_State* L) {
    return reinterpret_cast<int (__cdecl*)(lua_State*)>(kLuaGetTop)(L);
}
inline int lua_type(lua_State* L, int index) {
    return reinterpret_cast<int (__cdecl*)(lua_State*, int)>(kLuaType)(L, index);
}
inline double lua_tonumber(lua_State* L, int index) {
    return reinterpret_cast<double (__cdecl*)(lua_State*, int)>(kLuaToNumber)(L, index);
}
inline int lua_toboolean(lua_State* L, int index) {
    return reinterpret_cast<int (__cdecl*)(lua_State*, int)>(kLuaToBoolean)(L, index);
}
inline void lua_pushnumber(lua_State* L, double value) {
    reinterpret_cast<void (__cdecl*)(lua_State*, double)>(kLuaPushNumber)(L, value);
}
inline void lua_pushstring(lua_State* L, const char* value) {
    reinterpret_cast<void (__cdecl*)(lua_State*, const char*)>(kLuaPushString)(L, value);
}

// ---------------------------------------------------------------------------
// Hook sites: address, the stock bytes that must be there, and how many of
// them make up whole position-independent instructions.
// tools/verify_sites.py checks each address/Bytes pair against a Wow.exe.
// ---------------------------------------------------------------------------

// int __cdecl (int, int, int, float elapsed): runs once per frame, updating
// and then drawing the whole UI including the world.
constexpr uintptr_t kFrameHandler = 0x00495810;
constexpr uint8_t kFrameHandlerBytes[] = {0x55, 0x8B, 0xEC, 0xD9, 0x45, 0x14};

// void __thiscall CGCamera(target, float* yaw, float* pitch, float* roll):
// works out the camera's angles for this frame and builds its matrix.
constexpr uintptr_t kCameraOrientation = 0x00604490;
constexpr uint8_t kCameraOrientationBytes[] = {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x0C};

// uint32_t __thiscall CGCamera(Vec3* target, float* distance, float* height,
// Vec3* offset, float, float*): the distance for this frame after collision.
// Starts from the larger of the camera's zoom and zoom-blend fields. Returns
// flag bits, which the caller ORs into the camera's flags. The caller then
// places the camera that far behind the target along the matrix's forward
// axis, and compares the distance with the zoom field to see whether a
// collision has shortened it.
constexpr uintptr_t kCameraCollision = 0x00605D60;
constexpr uint8_t kCameraCollisionBytes[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x48};

// The final `ret` of the routine (starting 0x25 bytes earlier) that registers
// the last block of the game's own Lua functions each time the UI is loaded.
constexpr uintptr_t kLuaRegistrationReturn = 0x00530F85;
constexpr uint8_t kLuaRegistrationReturnBytes[] = {0xC3, 0xCC, 0xCC, 0xCC, 0xCC};
constexpr size_t kLuaRegistrationLength = 0x25;  // bytes from the routine's start to that ret

// The rest are `call` instructions inside larger routines, redirected.

// In the scene update: void __cdecl (), finds the building the camera is in
// (0x00795D40) and fills in the kSceneCamera... values.
constexpr uintptr_t kLocateCameraCall = 0x00783381;
constexpr uint8_t kLocateCameraCallBytes[] = {0xE8, 0xBA, 0x29, 0x01, 0x00};

// In the scene draw, back to back once everything in view has been found:
// void __cdecl () (0x0079A260), which goes through the visited groups for
// the scenery and creatures in them, and void __cdecl () (0x00793450), the
// last step before anything is drawn.
constexpr uintptr_t kFinishVisitsCall = 0x0079AB2A;
constexpr uint8_t kFinishVisitsCallBytes[] = {0xE8, 0x31, 0xF7, 0xFF, 0xFF};
constexpr uintptr_t kBeforeDrawingCall = 0x0079AB2F;
constexpr uint8_t kBeforeDrawingCallBytes[] = {0xE8, 0x1C, 0x89, 0xFF, 0xFF};

// In the cursor's hit test (0x004F9DA0), which first sets the device's
// projection afresh from the camera:
// int __thiscall CGWorldFrame(float x, float y, Vec3* start, Vec3* end)
// (0x004F6450), the cursor's ray from the near to the far end of the view;
// returns 0 if the cursor is not over the world.
constexpr uintptr_t kCursorRayCall = 0x004F9EC1;
constexpr uint8_t kCursorRayCallBytes[] = {0xE8, 0x8A, 0xC5, 0xFF, 0xFF};

}  // namespace game
