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
// so view space has z pointing into the screen and clip-space w is that
// depth. The Direct3D device (0x006A9B40) derives a second, "hardware" matrix
// from it for Direct3D's depth range of 0 to w: m[2][2] becomes f/(f-n) and
// m[3][2] becomes -fn/(f-n), nothing else changes. That one is what the
// vertex shaders multiply by (0x00872C10 hands it to them), so a vertex
// leaves a shader as p * view * hardware.
//
// The world's view matrix is a plain rotation: the client draws relative to
// the camera, so a point p of the world is (p - eye) * view in view space.
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

// The two Direct3D 9 devices the client has (gxApi "d3d9" and "d3d9ex"),
// told apart from its OpenGL one by the virtual table an object starts with.
// The second class derives from the first and keeps its fields.
constexpr uintptr_t kGxDeviceClassDirect3D9 = 0x00A2E718;
constexpr uintptr_t kGxDeviceClassDirect3D9Ex = 0x00A2F500;

inline bool IsDirect3DDevice(const uint8_t* device) {
    const uintptr_t virtuals = *reinterpret_cast<const uintptr_t*>(device);
    return virtuals == kGxDeviceClassDirect3D9 || virtuals == kGxDeviceClassDirect3D9Ex;
}

// In a Direct3D device:
constexpr size_t kGxDeviceDirect3D = 0x397C;  // IDirect3DDevice9*

// The engine's render states: an array of 0x18-byte records, value first,
// holding what the next draw is to be made with. Just before each draw the
// device sends Direct3D whatever has changed (0x006A5940, 0x006A9860).
constexpr size_t kGxDeviceStates = 0x28F4;    // pointer to the records
constexpr size_t kGxStateSize = 0x18;
constexpr size_t kGxStateClipPlanes = 18;     // uint32_t: a bit for each user clip plane that is on
constexpr size_t kGxStateVertexShader = 77;   // the vertex shader, or null for a draw without one

// What the device last sent Direct3D for the clip-plane state
// (D3DRS_CLIPPLANEENABLE): it sends again only when the state above differs
// from this (0x006A50E4). All ones until it has sent anything. The client
// itself uses plane 0 alone, for a few models (0x0081FD6B).
constexpr size_t kGxDeviceSentClipPlanes = 0x3E84;  // uint32_t
constexpr uint32_t kGxNothingSent = 0xFFFFFFFF;

// IDirect3DDevice9, as far as it is used here. COM: the object is the first
// argument of each method, __stdcall, and a negative result means failure.
constexpr size_t kDirect3DClear = 43;           // (count, rects, flags, colour, float z, stencil)
constexpr size_t kDirect3DSetClipPlane = 55;    // (index, const float plane[4])
constexpr size_t kDirect3DSetRenderState = 57;  // (state, value)
constexpr uint32_t kDirect3DClearDepth = 0x2;        // D3DCLEAR_ZBUFFER
constexpr uint32_t kDirect3DClipPlaneEnable = 152;   // D3DRS_CLIPPLANEENABLE

inline void** Direct3DVirtuals(void* direct3d) {
    return *reinterpret_cast<void***>(direct3d);
}

// With a vertex shader, a user clip plane is in clip space: a vertex is kept
// if its clip-space position (x, y, z, w) dotted with the plane is not
// negative. Depth is not touched by it.
inline long Direct3DSetClipPlane(void* direct3d, uint32_t index, const float* plane) {
    return reinterpret_cast<long (__stdcall*)(void*, uint32_t, const float*)>(
        Direct3DVirtuals(direct3d)[kDirect3DSetClipPlane])(direct3d, index, plane);
}

inline long Direct3DSetRenderState(void* direct3d, uint32_t state, uint32_t value) {
    return reinterpret_cast<long (__stdcall*)(void*, uint32_t, uint32_t)>(
        Direct3DVirtuals(direct3d)[kDirect3DSetRenderState])(direct3d, state, value);
}

// Clears the depth buffer alone, within the viewport, to `depth` (0 near, 1 far).
inline long Direct3DClearDepth(void* direct3d, float depth) {
    return reinterpret_cast<long (__stdcall*)(void*, uint32_t, const void*, uint32_t, uint32_t, float, uint32_t)>(
        Direct3DVirtuals(direct3d)[kDirect3DClear])(direct3d, 0, nullptr, kDirect3DClearDepth, 0, depth, 0);
}

// ---------------------------------------------------------------------------
// The world scene
//
// Once per frame the world frame (0x004FAF90) updates the camera, sets the
// device's view and projection from it (0x00607CB0), updates the scene
// (0x007831A0), draws the world (0x004F8EA0) and puts the device's matrices
// back. Drawing the world is: the scene (0x0079A870), then see-through
// models and effects, ending with a sorted list of them (0x004F920B); and
// after those the sun's glare, floating text, names over heads and the
// full-screen effects.
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
constexpr uintptr_t kSceneCameraGroups = 0x00CDB0DC;           // uint32_t*: their indexes (the list the walk gets
                                                               // starts 8 bytes before: capacity, count, data)
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
constexpr size_t kBuildingNameSet = 0x104;    // uint32_t: which of the model's sets of area names this placement uses
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
constexpr size_t kPlacedGroupQueued = 0xB4;  // nonzero while the group is on this frame's list of groups to
                                             // draw (0x00CDB080; it is the list's link): the visit looks at
                                             // this to queue it once

// A building's model.
constexpr size_t kModelHeader = 0x120;      // pointer to the model file's header
constexpr size_t kModelGroupInfo = 0x130;   // array of 0x20-byte records, one per group, flags first
constexpr size_t kModelDoorways = 0x13C;    // array of 8-byte records, see below
constexpr size_t kModelGroupCount = 0x16C;  // uint32_t
constexpr size_t kModelLoaded = 0x1E0;      // uint32_t: nonzero once the model file is in
constexpr size_t kModelGroups = 0x1F8;      // array of pointers to the groups' own data
constexpr size_t kModelGroupInfoSize = 0x20;

constexpr size_t kModelHeaderId = 0x20;     // uint32_t, in the header: the model's number in the area table below

// A group's own data.
constexpr size_t kGroupDataFlags = 0x30;          // uint32_t: the same flags as in the model's record
constexpr size_t kGroupDataFirstDoorway = 0x50;   // uint32_t: where the group's doorways start in kModelDoorways
constexpr size_t kGroupDataDoorwayCount = 0x54;   // uint32_t
constexpr size_t kGroupDataAreaId = 0x180;        // uint32_t: the group's number in the area table below
constexpr size_t kGroupDataLoadFlags = 0x198;     // uint32_t
constexpr uint32_t kGroupDataLoaded = 0x1;

// One side of a doorway ("portal"): each group lists its own, and the
// group on the other side lists the same doorway from there.
struct Doorway {
    uint16_t surface;  // which opening, among the model's
    uint16_t group;    // the group on the other side; 0xFFFF for none
    int16_t side;
    uint16_t unused;
};
constexpr uint16_t kNoGroup = 0xFFFF;

// Group flags, from the model file.
constexpr uint32_t kGroupFlagOutdoor = 0x8;             // drawn with the outdoor world
constexpr uint32_t kGroupFlagOutdoorLit = 0x40;         // indoors, but lit by the sky
constexpr uint32_t kGroupFlagAlwaysOutdoor = 0x10000;   // has no doorways; drawn whenever it is in view

// Whether a place inside a building counts as outdoors is not the group
// flag alone. A city's streets and squares are often groups without it:
// rooms as far as drawing goes, entered through doorways, only without a
// ceiling. The client's own answer (0x0077FBF0, behind IsIndoors() and
// IsOutdoors(), and so behind where one may ride) goes by WMOAreaTable.dbc
// first: the record for (model, name set, group) says "outdoors" with flag
// 4, "indoors" with flag 2, and only where neither is set, or there is no
// record, does the group flag decide.
//
// const void* __cdecl (uint32_t model, uint32_t nameSet, uint32_t group):
// the record, or null.
constexpr uintptr_t kFindBuildingArea = 0x00990560;
constexpr size_t kAreaFlags = 0x24;  // uint32_t, in the record
constexpr uint32_t kAreaFlagIndoors = 0x2;
constexpr uint32_t kAreaFlagOutdoors = 0x4;

inline const void* FindBuildingArea(uint32_t model, uint32_t nameSet, uint32_t group) {
    return reinterpret_cast<const void* (__cdecl*)(uint32_t, uint32_t, uint32_t)>(kFindBuildingArea)(
        model, nameSet, group);
}

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

// The same against everything a click can land on: what the client's click
// test passes (0x004F9B5C). Terrain, buildings as far as they block
// movement, models, game objects.
constexpr uint32_t kIntersectClick = 0x00100151;

// bool __cdecl (const Vec3* from, const Vec3* to, float fraction,
// void* buildings[2], uint32_t groups[4]): the building group whose surface
// the line meets first, among the placed buildings. The client asks this
// with a line straight down from the camera to learn which room the camera
// is in (0x00795D40); it is a plain line-against-surfaces test and takes
// any line. `fraction` limits how far along the line counts (1: all of
// it). Returns false, and null in buildings[0], if the line meets nothing
// or if what it meets first is a group with kGroupFlagOutdoor. Otherwise
// buildings[0] is the placed building and groups[0] the group, which may
// well be a street: see the note on what counts as outdoors above.
constexpr uintptr_t kFindGroupOnLine = 0x007D59B0;

inline bool FindGroupOnLine(const Vec3& from, const Vec3& to, float fraction, void** buildings, uint32_t* groups) {
    return reinterpret_cast<bool (__cdecl*)(const Vec3*, const Vec3*, float, void**, uint32_t*)>(
        kFindGroupOnLine)(&from, &to, fraction, buildings, groups);
}

// bool __cdecl (const Vec3* from, const Vec3* to, float* fraction,
// uint32_t flags, void** chunk): whether the line meets the terrain, and how
// far along. The client's camera lookup asks this first, with flags 0x100,
// and lets the building lookup count only up to there: a mine under a hill
// is not what someone walking on the hill is in.
constexpr uintptr_t kIntersectTerrain = 0x007A39F0;
constexpr float kCameraLookupDepth = 1760.0f;  // yards straight down the client's lookup goes

inline bool IntersectTerrain(const Vec3& from, const Vec3& to, float* fraction) {
    void* chunk = nullptr;
    *fraction = 1.0f;
    return reinterpret_cast<bool (__cdecl*)(const Vec3*, const Vec3*, float*, uint32_t, void**)>(
        kIntersectTerrain)(&from, &to, fraction, 0x100, &chunk);
}

// uint32_t: world switches. With this bit clear the client draws and tests
// no buildings at all, and asks none of the questions above.
constexpr uintptr_t kWorldSwitches = 0x00CD774C;
constexpr uint32_t kWorldSwitchBuildings = 0x100;

// While the scene is drawn from inside a building, two rectangles of the
// screen (as fractions, 0 to 1) through which the outdoor world shows:
// doorways and windows the walk through the rooms has come across. They are
// emptied in every scene update (0x00795400). After the walk, the first is
// what decides whether the sky is drawn and which colour the screen is
// cleared to, and weather and light look at it too; the second is handed to
// the outdoor culling (0x0079A790), which is skipped if it is empty.
struct OutdoorWindow {
    float left, top, right, bottom;
    float depth;  // below zero: the outdoor world does not show at all
};
constexpr uintptr_t kSceneSkyWindow = 0x00ADF570;
constexpr uintptr_t kSceneOutdoorWindow = 0x00ADF58C;
constexpr OutdoorWindow kNoOutdoorWindow = {3.4028234664e38f, 3.4028234664e38f, -3.4028234664e38f,
                                            -3.4028234664e38f, -1.0f};

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
// What is drawn, piece by piece
// ---------------------------------------------------------------------------

// This frame's list of placed groups to draw (the visit puts them on it):
// the head, and where in a placed group the link sits. The link's second
// word, kPlacedGroupQueued, is the next group; the list ends at a null or
// odd value.
constexpr uintptr_t kQueuedGroupsHead = 0x00CDB088;
constexpr uintptr_t kQueuedGroupsLinkOffset = 0x00CDB080;  // int32_t

constexpr size_t kPlacedGroupOwner = 0x20;   // in a placed group: a link whose word at kOwnerBuilding is the
constexpr size_t kOwnerBuilding = 0x08;      // placed building it belongs to
constexpr size_t kBuildingPlacement = 0xB0;  // in a placed building: Matrix, world coordinates to the model's own

// A piece of scenery or a creature, as the scene files it under the places
// it stands in (a placed group keeps a list of each, at +0x78 and +0x84).
constexpr size_t kSceneModel = 0x34;   // its model, null while that is loading
constexpr size_t kSceneCentre = 0x38;  // Vec3, world coordinates: the centre of the sphere it is tested by
constexpr size_t kSceneRadius = 0x44;  // float

// Whether a line meets one building group's surfaces. What the client does
// for each group of a building a line passes (0x007AECB0, and 0x007D59B0 for
// the camera's room): the line is taken to the model's coordinates
// (kBuildingPlacement), put to the group's box, and then to its surfaces.
//
// bool __thiscall Model(const Vec3* from, const Vec3* to, uint32_t group)
// (0x007AE880): whether the line touches the group's box. False as well for
// a model or a group whose data is not loaded.
constexpr uintptr_t kLineTouchesGroupBox = 0x007AE880;
// bool __thiscall GroupData(const Vec3 line[2], float* fraction, uint32_t
// flags, uint32_t surfaces, uint8_t* out, void* building) (0x007CB0C0).
// `fraction` goes in as how far along the line counts and comes back as how
// far along the hit is. `flags` are the line test's own (only its liquid
// bits are looked at here); `surfaces` is the mask of surface kinds to pass
// over, which 0x007AE140 makes from the flags: 0x82 for kIntersectBuildings.
// The client zeroes five words of scratch (kLineScratch) before every call.
constexpr uintptr_t kLineMeetsGroup = 0x007CB0C0;
constexpr uintptr_t kLineScratch = 0x00CB7528;
constexpr size_t kLineScratchWords = 5;
constexpr uint32_t kBuildingSurfacesPassed = 0x82;

inline bool LineTouchesGroupBox(void* model, const Vec3* from, const Vec3* to, uint32_t group) {
    return reinterpret_cast<bool (__thiscall*)(void*, const Vec3*, const Vec3*, uint32_t)>(kLineTouchesGroupBox)(
        model, from, to, group);
}

inline bool LineMeetsGroup(void* groupData, const Vec3* line, float* fraction, uint32_t flags, uint32_t surfaces,
                           uint8_t* out, void* building) {
    uint32_t* scratch = reinterpret_cast<uint32_t*>(kLineScratch);
    for (size_t i = 0; i < kLineScratchWords; ++i) {
        scratch[i] = 0;
    }
    return reinterpret_cast<bool (__thiscall*)(void*, const Vec3*, float*, uint32_t, uint32_t, uint8_t*, void*)>(
        kLineMeetsGroup)(groupData, line, fraction, flags, surfaces, out, building);
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

// In the scene draw, when the camera stands in a building:
// void __thiscall PlacedBuilding(void* groups) (0x007B3B20), the walk from
// the camera's groups through doorways to every room in view (0x007AD1F0,
// 0x007AC060). It visits each of the camera's groups under the whole-screen
// view, and from each goes on through the doorways, judging them from the
// client's camera position (kSceneCameraPosition): a doorway whose far side
// the camera is on is not gone through, and a room beyond one is visited
// under a view narrowed to the doorway. A doorway to an outdoor group opens
// kSceneOutdoorWindow, which the client looks at right after the walk to
// see whether to draw the outdoor world as well; then (0x00799F80) it deals
// creatures and objects out among the groups visited so far.
constexpr uintptr_t kWalkRoomsCall = 0x0079AA2C;
constexpr uint8_t kWalkRoomsCallBytes[] = {0xE8, 0xEF, 0x90, 0x01, 0x00};

// In the scene draw, after kBeforeDrawingCall: void __cdecl (uint32_t what,
// uint32_t colour) (0x006813B0), clears the screen. `what` is 3, colour and
// depth; the colour is the fog's when no sky shows. Depth is cleared to 1,
// the far end (0x006A74B0).
constexpr uintptr_t kSceneClearCall = 0x0079AB9A;
constexpr uint8_t kSceneClearCallBytes[] = {0xE8, 0x11, 0x68, 0xEE, 0xFF};

// In the two Direct3D devices' draw routines (0x006A3620, 0x006A77C0), which
// every draw goes through: void __thiscall CGxDevice() (0x006A5940,
// 0x006A9860), which sends Direct3D the engine's render states, shaders and
// matrices for the draw. The draw itself follows.
constexpr uintptr_t kDrawSyncCall = 0x006A3642;
constexpr uint8_t kDrawSyncCallBytes[] = {0xE8, 0xF9, 0x22, 0x00, 0x00};
constexpr uintptr_t kDrawSyncExCall = 0x006A77E2;
constexpr uint8_t kDrawSyncExCallBytes[] = {0xE8, 0x79, 0x20, 0x00, 0x00};

// In the world's drawing (0x004F8EA0), after the scene and the see-through
// models: void __cdecl (uint32_t list, const Vec3* from) (0x00682960), draws
// the sorted list of effects. It is the last of the world drawn as solid
// things in their places; the sun's glare, floating text and names follow.
constexpr uintptr_t kWorldDrawnCall = 0x004F920B;
constexpr uint8_t kWorldDrawnCallBytes[] = {0xE8, 0x50, 0x97, 0x18, 0x00};

// In the cursor's hit test (0x004F9DA0), which first sets the device's
// projection afresh from the camera:
// int __thiscall CGWorldFrame(float x, float y, Vec3* start, Vec3* end)
// (0x004F6450), the cursor's ray from the near to the far end of the view;
// returns 0 if the cursor is not over the world.
constexpr uintptr_t kCursorRayCall = 0x004F9EC1;
constexpr uint8_t kCursorRayCallBytes[] = {0xE8, 0x8A, 0xC5, 0xFF, 0xFF};

// In the scene's drawing of the queued groups (0x007964A0), once for each:
// void __thiscall Model(uint32_t group, const Matrix* placement, void* views)
// (0x007ABF50, which has no other caller), draws one group of a building.
// `placement` is the placed building's own matrix, kBuildingPlacement into it.
constexpr uintptr_t kDrawGroupCall = 0x007967AA;
constexpr uint8_t kDrawGroupCallBytes[] = {0xE8, 0xA1, 0x57, 0x01, 0x00};

// Where the scene goes through a visited group's scenery (0x00799B70) and
// its creatures and objects (0x00793270), for each one:
// int __thiscall View(const Vec3* centre, float radius) (0x00983FB0, which
// has no other caller), nonzero if the sphere shows in that view. `centre`
// points into the piece itself, at kSceneCentre. A piece that shows in none
// of the group's views is not drawn.
constexpr uintptr_t kSceneryInViewCall = 0x00799C52;
constexpr uint8_t kSceneryInViewCallBytes[] = {0xE8, 0x59, 0xA3, 0x1E, 0x00};
constexpr uintptr_t kCreatureInViewCall = 0x007932F2;
constexpr uint8_t kCreatureInViewCallBytes[] = {0xE8, 0xB9, 0x0C, 0x1F, 0x00};

}  // namespace game
