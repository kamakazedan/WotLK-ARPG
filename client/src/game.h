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
// World frame (the 3D view and everything clicked in it)
// ---------------------------------------------------------------------------

constexpr uintptr_t kWorldFramePointer = 0x00B7436C;  // CGWorldFrame*

inline uint8_t* WorldFrame() {
    return *reinterpret_cast<uint8_t* const volatile*>(kWorldFramePointer);
}

// What the last capture found under the cursor.
enum HitKind : int {
    kHitNothing = 0,
    kHitTerrain = 1,
    kHitObject = 2,       // a unit or game object, by guid
    kHitWorldObject = 3,  // a building or doodad; clicks treat it as terrain
};

constexpr size_t kWorldFrameHitKind = 0x2D8;  // HitKind
constexpr size_t kWorldFrameCamera = 0x7E20;  // CGCamera*

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

// ---------------------------------------------------------------------------
// Camera
// ---------------------------------------------------------------------------

inline void* ActiveCamera() {
    uint8_t* worldFrame = WorldFrame();
    return worldFrame != nullptr ? At<void*>(worldFrame, kWorldFrameCamera) : nullptr;
}

constexpr size_t kCameraFlags = 0x98;      // uint32_t
constexpr size_t kCameraZoom = 0x118;      // float: wanted distance from the character
constexpr size_t kCameraZoomBlend = 0x1E8; // float: distance being blended towards or from

constexpr uint32_t kCameraFlagModelDriven = 0x4;  // camera follows a model animation
constexpr uint32_t kCameraFlagNoCollision = 0x8;  // the collision routine returns early when set

// void __thiscall CGCamera(float yaw, float pitch, float roll): builds the
// camera's rotation matrix from angles in radians. Yaw 0 looks north (+X) and
// grows counter-clockwise; positive pitch looks down.
constexpr uintptr_t kBuildCameraMatrix = 0x00607BD0;

inline void BuildCameraMatrix(void* camera, float yaw, float pitch, float roll) {
    reinterpret_cast<void (__thiscall*)(void*, float, float, float)>(kBuildCameraMatrix)(camera, yaw, pitch, roll);
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
constexpr uintptr_t kCameraCollision = 0x00605D60;
constexpr uint8_t kCameraCollisionBytes[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x48};

// The final `ret` of the routine (starting 0x25 bytes earlier) that registers
// the last block of the game's own Lua functions each time the UI is loaded.
constexpr uintptr_t kLuaRegistrationReturn = 0x00530F85;
constexpr uint8_t kLuaRegistrationReturnBytes[] = {0xC3, 0xCC, 0xCC, 0xCC, 0xCC};
constexpr size_t kLuaRegistrationLength = 0x25;  // bytes from the routine's start to that ret

}  // namespace game
