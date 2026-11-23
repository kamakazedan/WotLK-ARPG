// WotLK-ARPG client library.
//
// What the Lua sandbox cannot do: move by holding the mouse button, pin the
// camera, and show the building the character is in from inside (that last
// one is in cutaway.cpp). The library keeps no settings of its own; the addon
// pushes them in through the Lua functions registered at the bottom of this
// file, and they are dropped again whenever the UI reloads.

#include "cutaway.h"
#include "game.h"
#include "hook.h"
#include "log.h"
#include "win.h"

#define ARPG_VERSION "0.6.0"

namespace {

constexpr float kRadiansPerDegree = 0.017453292f;

// Reported to the addon so it can say which parts are working.
enum Feature : uint32_t {
    kFeatureLua = 1,
    kFeatureHoldMove = 2,
    kFeatureCameraAngles = 4,
    kFeatureCameraDistance = 8,
    kFeatureCutaway = 16,
    kFeatureInsideView = 32,  // the cutaway shows buildings from inside (0.4.0); set together with 16
    kFeatureRooms = 64,       // ... and tells rooms from streets, and reports what it found (0.5.0)
};
uint32_t g_features = 0;

float Clamp(float value, float low, float high) {
    return value < low ? low : (value > high ? high : value);
}

// The player's own character's position (its feet). False outside the world.
bool PlayerPosition(game::Vec3* out) {
    if (!game::InWorld()) {
        return false;
    }
    void* player = game::PlayerObject();
    if (player == nullptr) {
        return false;
    }
    game::ObjectPosition(player, out);
    return true;
}

// ---------------------------------------------------------------------------
// Camera lock
//
// The client works out the camera's angles and its distance afresh every
// frame, in two routines. Both are hooked.
//
// Angles: the client's routine runs, then its results are replaced with the
// locked ones and the camera's matrix is rebuilt from them. The client's own
// yaw and pitch are left as they were, so mouse-look still changes them, out
// of sight, and they are what the camera shows again when the lock goes.
//
// Distance: while locked, the camera's own zoom is kept at the locked
// distance, because the rest of the client asks the zoom, not the camera's
// position, how far away the camera is (to hide the character in first
// person, for one). The player's own zoom is put back when the lock goes.
// Collision is switched off by raising the camera's first-person flag for the
// length of the client's distance routine, which makes it hand back the zoom
// untested.
// ---------------------------------------------------------------------------

struct CameraLock {
    bool locked;
    bool collide;
    float yaw;       // radians, 0 = looking north
    float pitch;     // radians, positive = looking down
    float distance;  // yards from the character

    void* driven;    // the camera whose zoom is currently ours, if any
    float userZoom;  // its zoom before the lock took hold
};
CameraLock g_camera = {false, false, 0.0f, 0.0f, 0.0f, nullptr, 0.0f};

// For the addon's status display: proof that the lock is actually being
// applied by the client, not just asked for.
struct CameraStats {
    uint32_t angleCalls;
    uint32_t angleLocked;
    uint32_t distanceCalls;
    uint32_t distanceLocked;
};
CameraStats g_cameraStats = {0, 0, 0, 0};

bool CameraIsLocked(void* camera) {
    return g_camera.locked && camera == game::ActiveCamera();
}

using CameraOrientation = void (__thiscall*)(void* camera, void* target, float* yaw, float* pitch, float* roll);
CameraOrientation g_originalCameraOrientation = nullptr;

// __fastcall with an unused second parameter is how a free function receives
// a __thiscall call: `this` in ecx, the rest on the stack, callee cleans up.
void __fastcall CameraOrientationHook(void* camera, void* /*edx*/, void* target, float* yaw, float* pitch,
                                      float* roll) {
    ++g_cameraStats.angleCalls;
    g_originalCameraOrientation(camera, target, yaw, pitch, roll);
    // Without a target the client's routine builds no matrix either.
    if (target != nullptr && CameraIsLocked(camera)) {
        ++g_cameraStats.angleLocked;
        *yaw = g_camera.yaw;
        *pitch = g_camera.pitch;
        game::BuildCameraMatrix(camera, g_camera.yaw, g_camera.pitch, *roll);
    }
}

using CameraCollision = uint32_t (__thiscall*)(void* camera, float* target, float* distance, float* height,
                                               float* offset, float scale, float* fraction);
CameraCollision g_originalCameraCollision = nullptr;

uint32_t __fastcall CameraCollisionHook(void* camera, void* /*edx*/, float* target, float* distance,
                                        float* height, float* offset, float scale, float* fraction) {
    ++g_cameraStats.distanceCalls;
    float& zoom = game::At<float>(camera, game::kCameraZoom);
    float& zoomBlend = game::At<float>(camera, game::kCameraZoomBlend);
    uint32_t& flags = game::At<uint32_t>(camera, game::kCameraFlags);

    if (!CameraIsLocked(camera)) {
        if (g_camera.driven == camera) {
            // The lock has just gone: the player's own zoom comes back.
            zoom = g_camera.userZoom;
            zoomBlend = g_camera.userZoom;
            g_camera.driven = nullptr;
        }
        return g_originalCameraCollision(camera, target, distance, height, offset, scale, fraction);
    }

    if (g_camera.driven != camera) {
        // The lock has just taken hold: remember the player's own zoom.
        g_camera.userZoom = zoom > zoomBlend ? zoom : zoomBlend;
        g_camera.driven = camera;
    }
    ++g_cameraStats.distanceLocked;
    zoom = g_camera.distance;
    zoomBlend = g_camera.distance;

    if (g_camera.collide) {
        // A real collision test, from the locked distance inwards. A stale
        // first-person flag would skip the test, so it goes first.
        flags &= ~game::kCameraFlagFirstPerson;
        return g_originalCameraCollision(camera, target, distance, height, offset, scale, fraction);
    }

    // No collision: with the flag up the routine returns the zoom as it is.
    // The flag comes down again before the caller looks at it.
    flags |= game::kCameraFlagFirstPerson;
    const uint32_t result = g_originalCameraCollision(camera, target, distance, height, offset, scale, fraction);
    flags &= ~game::kCameraFlagFirstPerson;
    return result & ~game::kCameraFlagFirstPerson;
}

// ---------------------------------------------------------------------------
// Ground clicks
//
// From a camera high above, the cursor's ray often meets a roof or a treetop
// before it meets the ground the player means. The client keeps the ray it
// used for the last capture, so the point where that ray comes down to the
// height of the character's feet can be worked out: where the ground would be
// if nothing stood in the way. Two uses:
//   - a hit on the static world more than `maxRise` above the character's
//     feet is moved to that point before the click is acted on;
//   - a held button keeps the character walking towards that point while the
//     cursor passes over a creature, instead of stopping.
// On foot only x and y decide when the character has arrived, so it does not
// matter that the real ground there may be a little higher or lower.
// ---------------------------------------------------------------------------

struct GroundClicks {
    bool enabled;
    float maxRise;  // yards above the character's feet that still counts as somewhere to walk
};
GroundClicks g_ground = {false, 3.0f};

constexpr float kGroundClickReach = 300.0f;  // yards; further than this the ray is too flat to trust

// Where the cursor's ray of the last capture passes through the height of
// `feet`. Worked out from the ray's higher end down to its lower one, so it
// does not matter which end the client calls the start.
bool GroundPointUnderCursor(uint8_t* worldFrame, const game::Vec3& feet, game::Vec3* out) {
    const game::Vec3& first = game::At<game::Vec3>(worldFrame, game::kWorldFrameRayStart);
    const game::Vec3& second = game::At<game::Vec3>(worldFrame, game::kWorldFrameRayEnd);
    const game::Vec3& high = first.z >= second.z ? first : second;
    const game::Vec3& low = first.z >= second.z ? second : first;
    const float fall = high.z - low.z;
    if (fall < 0.5f) {
        return false;  // a level ray never comes down to the ground
    }
    const float along = (high.z - feet.z) / fall;  // 0 at the high end, 1 at the low end
    if (along <= 0.0f || along > 1.0f) {
        return false;  // the ray does not pass through that height
    }
    const float x = high.x + (low.x - high.x) * along;
    const float y = high.y + (low.y - high.y) * along;
    const float dx = x - feet.x;
    const float dy = y - feet.y;
    if (dx * dx + dy * dy > kGroundClickReach * kGroundClickReach) {
        return false;
    }
    out->x = x;
    out->y = y;
    out->z = feet.z;
    return true;
}

// Moves a captured hit that is too high to walk to down to the ground.
void LowerClickToGround(uint8_t* worldFrame) {
    if (game::HitIsOnTransport(worldFrame)) {
        return;  // its position is in the transport's coordinates, not the world's
    }
    game::Vec3 feet;
    if (!PlayerPosition(&feet)) {
        return;
    }
    game::Vec3& hit = game::At<game::Vec3>(worldFrame, game::kWorldFrameHitPosition);
    if (hit.z - feet.z <= g_ground.maxRise) {
        return;  // already somewhere to walk
    }
    game::Vec3 ground;
    if (GroundPointUnderCursor(worldFrame, feet, &ground)) {
        hit = ground;
    }
}

// ---------------------------------------------------------------------------
// Hold to move, and moving to a given spot
//
// The addon binds the left mouse button to ARPG_HoldStart / ARPG_HoldStop.
// Those only set flags. The work happens in the frame handler, outside any
// addon code, using the client's own click handling: capture what is under
// the cursor, then act on it as a right click would. On the ground that is
// the client's own click-to-move, repeated while the button is held.
// ---------------------------------------------------------------------------

struct HoldMove {
    bool held;
    bool pressPending;  // a press not yet acted on (survives a release in the same frame)
    bool walking;       // this hold began on the ground: it is a walk, and repeats
    uint32_t lastTick;
    uint32_t intervalMs;
};
HoldMove g_hold = {false, false, false, 0, 100};

struct MoveRequest {
    bool pending;
    bool hasHeight;
    game::Vec3 position;
};
MoveRequest g_move = {false, false, {0.0f, 0.0f, 0.0f}};

void HoldMoveTick() {
    if (!g_hold.held && !g_hold.pressPending) {
        return;
    }
    uint8_t* worldFrame = game::WorldFrame();
    if (!game::InWorld() || worldFrame == nullptr) {
        g_hold.held = false;
        g_hold.pressPending = false;
        return;
    }

    const uint32_t now = GetTickCount();
    const bool press = g_hold.pressPending;
    if (!press && now - g_hold.lastTick < g_hold.intervalMs) {
        return;
    }
    g_hold.pressPending = false;
    g_hold.lastTick = now;

    game::CaptureDefaultAction(worldFrame);
    const int kind = game::At<int>(worldFrame, game::kWorldFrameHitKind);
    if (kind == game::kHitNothing) {
        return;
    }
    // A spell waiting for its target gets the press the way a left click
    // gives it: the reticle is placed, or the unit is chosen. A right click
    // would cancel the spell, and the character should not walk off either.
    if (game::SpellIsTargeting()) {
        if (press) {
            g_hold.walking = false;
            game::PerformDefaultAction(worldFrame, game::kClickLeft);
        }
        return;
    }
    if (press) {
        g_hold.walking = kind != game::kHitObject;
        if (!g_hold.walking) {
            // Attack, talk, loot: the client walks into range by itself.
            game::PerformDefaultAction(worldFrame, game::kClickRight);
            return;
        }
    } else if (!g_hold.walking) {
        // This hold began on a unit. Whatever the cursor is over by now, a
        // move from here would call the character off what it was sent to do.
        return;
    }

    // Walking. A held button never clicks a unit it passes over (nobody wants
    // a vendor opened in passing); it keeps on towards the ground under the
    // cursor instead.
    if (kind == game::kHitObject) {
        game::Vec3 feet;
        game::Vec3 ground;
        if (g_ground.enabled && PlayerPosition(&feet) && GroundPointUnderCursor(worldFrame, feet, &ground)) {
            game::ClickTerrain({0, 0, ground, game::kClickRight});
        }
        return;
    }
    if (g_ground.enabled) {
        LowerClickToGround(worldFrame);
    }
    game::PerformDefaultAction(worldFrame, game::kClickRight);
}

void MoveRequestTick() {
    if (!g_move.pending) {
        return;
    }
    g_move.pending = false;
    game::Vec3 feet;
    if (!PlayerPosition(&feet)) {
        return;
    }
    game::TerrainClick click = {0, 0, g_move.position, game::kClickRight};
    if (!g_move.hasHeight) {
        click.position.z = feet.z;
    }
    game::ClickTerrain(click);
}

using FrameHandler = int (__cdecl*)(int, int, int, int);
FrameHandler g_originalFrameHandler = nullptr;

int __cdecl FrameHandlerHook(int a1, int a2, int a3, int a4) {
    HoldMoveTick();
    MoveRequestTick();
    return g_originalFrameHandler(a1, a2, a3, a4);
}

// ---------------------------------------------------------------------------
// Lua functions for the addon
// ---------------------------------------------------------------------------

bool IsNumber(game::lua_State* L, int index) {
    return game::lua_type(L, index) == game::LUA_TNUMBER;
}

float NumberAt(game::lua_State* L, int index) {
    return static_cast<float>(game::lua_tonumber(L, index));
}

// version, features = ARPG_ClientVersion()
int __cdecl LuaClientVersion(game::lua_State* L) {
    game::lua_pushstring(L, ARPG_VERSION);
    game::lua_pushnumber(L, static_cast<double>(g_features));
    return 2;
}

// ARPG_HoldStart() / ARPG_HoldStop(): key down and key up of the move button.
int __cdecl LuaHoldStart(game::lua_State*) {
    g_hold.held = true;
    g_hold.pressPending = true;
    return 0;
}

int __cdecl LuaHoldStop(game::lua_State*) {
    g_hold.held = false;
    return 0;
}

// ARPG_SetHoldInterval(milliseconds): how often a held button re-issues the move.
int __cdecl LuaSetHoldInterval(game::lua_State* L) {
    if (IsNumber(L, 1)) {
        g_hold.intervalMs = static_cast<uint32_t>(Clamp(NumberAt(L, 1), 10.0f, 2000.0f));
    }
    return 0;
}

// ARPG_SetGroundClicks(maxRise) sends move clicks on roofs and treetops more
// than maxRise yards above the character to the ground beneath the cursor;
// ARPG_SetGroundClicks() leaves every click where the cursor's ray lands.
int __cdecl LuaSetGroundClicks(game::lua_State* L) {
    g_ground.enabled = IsNumber(L, 1);
    if (g_ground.enabled) {
        g_ground.maxRise = Clamp(NumberAt(L, 1), 0.5f, 50.0f);
    }
    return 0;
}

// ARPG_MoveTo(x, y [, z]): click-to-move to a spot in world coordinates, as
// a right click on that spot of ground would. Without z, the character's own
// height is used (on foot only x and y decide when it has arrived).
int __cdecl LuaMoveTo(game::lua_State* L) {
    if (!IsNumber(L, 1) || !IsNumber(L, 2)) {
        return 0;
    }
    g_move.position.x = NumberAt(L, 1);
    g_move.position.y = NumberAt(L, 2);
    g_move.hasHeight = IsNumber(L, 3);
    g_move.position.z = g_move.hasHeight ? NumberAt(L, 3) : 0.0f;
    g_move.pending = true;
    return 0;
}

// x, y, z = ARPG_PlayerPosition(): world coordinates of the character's feet
// (x north, y west, z up), or nothing outside the world.
int __cdecl LuaPlayerPosition(game::lua_State* L) {
    game::Vec3 feet;
    if (!PlayerPosition(&feet)) {
        return 0;
    }
    game::lua_pushnumber(L, feet.x);
    game::lua_pushnumber(L, feet.y);
    game::lua_pushnumber(L, feet.z);
    return 3;
}

// ARPG_SetCamera(yawDegrees, pitchDegrees, distance [, collide]) locks the
// camera; ARPG_SetCamera() with no numbers releases it.
int __cdecl LuaSetCamera(game::lua_State* L) {
    if (game::lua_gettop(L) < 3 || !IsNumber(L, 1) || !IsNumber(L, 2) || !IsNumber(L, 3)) {
        g_camera.locked = false;
        return 0;
    }
    g_camera.yaw = NumberAt(L, 1) * kRadiansPerDegree;
    g_camera.pitch = Clamp(NumberAt(L, 2), -89.0f, 89.0f) * kRadiansPerDegree;
    g_camera.distance = Clamp(NumberAt(L, 3), 2.0f, 200.0f);
    g_camera.collide = game::lua_gettop(L) >= 4 && game::lua_toboolean(L, 4) != 0;
    g_camera.locked = true;
    return 0;
}

// asked, angleCalls, angleLocked, distanceCalls, distanceLocked = ARPG_CameraInfo()
// `asked` is 1 while a lock is asked for. The counts are how often the client
// has run each of its two camera routines since the UI loaded, and in how
// many of those runs the lock was applied.
int __cdecl LuaCameraInfo(game::lua_State* L) {
    game::lua_pushnumber(L, g_camera.locked ? 1.0 : 0.0);
    game::lua_pushnumber(L, static_cast<double>(g_cameraStats.angleCalls));
    game::lua_pushnumber(L, static_cast<double>(g_cameraStats.angleLocked));
    game::lua_pushnumber(L, static_cast<double>(g_cameraStats.distanceCalls));
    game::lua_pushnumber(L, static_cast<double>(g_cameraStats.distanceLocked));
    return 5;
}

// Runs each time the client has finished registering its own Lua functions,
// which is once per UI load. A fresh UI starts from a clean slate: the addon
// has to ask again for anything it wants.
void __cdecl OnLuaStateCreated() {
    g_hold.held = false;
    g_hold.pressPending = false;
    g_hold.walking = false;
    g_move.pending = false;
    g_ground.enabled = false;
    g_camera.locked = false;
    g_cameraStats = {0, 0, 0, 0};
    cutaway::Reset();

    game::RegisterLuaFunction("ARPG_ClientVersion", LuaClientVersion);
    game::RegisterLuaFunction("ARPG_HoldStart", LuaHoldStart);
    game::RegisterLuaFunction("ARPG_HoldStop", LuaHoldStop);
    game::RegisterLuaFunction("ARPG_SetHoldInterval", LuaSetHoldInterval);
    game::RegisterLuaFunction("ARPG_SetGroundClicks", LuaSetGroundClicks);
    game::RegisterLuaFunction("ARPG_MoveTo", LuaMoveTo);
    game::RegisterLuaFunction("ARPG_PlayerPosition", LuaPlayerPosition);
    game::RegisterLuaFunction("ARPG_SetCamera", LuaSetCamera);
    game::RegisterLuaFunction("ARPG_CameraInfo", LuaCameraInfo);
    game::RegisterLuaFunction("ARPG_SetCutaway", cutaway::LuaSetCutaway);
    game::RegisterLuaFunction("ARPG_CutawayInfo", cutaway::LuaCutawayInfo);
}

// ---------------------------------------------------------------------------
// Start-up
// ---------------------------------------------------------------------------

bool IsExpectedExecutable() {
    const uint8_t* image = static_cast<const uint8_t*>(GetModuleHandleA(nullptr));
    if (reinterpret_cast<uintptr_t>(image) != game::kImageBase) {
        return false;
    }
    const uint32_t peOffset = *reinterpret_cast<const uint32_t*>(image + 0x3C);
    const uint32_t timestamp = *reinterpret_cast<const uint32_t*>(image + peOffset + 8);
    return timestamp == game::kBuildTimestamp;
}

void Attach(void* self) {
    logfile::Open();
    logfile::Write("WotLK-ARPG client library %s", ARPG_VERSION);

    if (!IsExpectedExecutable()) {
        logfile::Write("This is not Wow.exe 3.3.5a (build 12340). Nothing was changed.");
        return;
    }

    // The loader slot this library occupies normally loads awesome_wotlk.
    // If that is installed, load it first: hooks placed afterwards sit in
    // front of its hooks and both keep working. (If this file itself was
    // installed under that name, the call just returns this module.)
    void* awesome = LoadLibraryA("AwesomeWotlkLib.dll");
    if (awesome != nullptr && awesome != self) {
        logfile::Write("Loaded AwesomeWotlkLib.dll first.");
    }

    // Let Lua call functions that live outside Wow.exe.
    *reinterpret_cast<uint32_t*>(game::kLuaFunctionRangeMin) = 1;
    *reinterpret_cast<uint32_t*>(game::kLuaFunctionRangeMax) = 0xFFFFFFFF;

    logfile::Write("Hooks:");
    const hook::Site luaSite = {"Lua registration", game::kLuaRegistrationReturn, game::kLuaRegistrationReturnBytes,
                                sizeof(game::kLuaRegistrationReturnBytes), 0};
    if (hook::IsActive(hook::CallBeforeReturn(luaSite, OnLuaStateCreated))) {
        g_features |= kFeatureLua;
    }

    const hook::Site frameSite = {"Frame handler", game::kFrameHandler, game::kFrameHandlerBytes,
                                  sizeof(game::kFrameHandlerBytes), sizeof(game::kFrameHandlerBytes)};
    if (hook::IsActive(hook::Detour(frameSite, reinterpret_cast<void*>(FrameHandlerHook),
                                    reinterpret_cast<void**>(&g_originalFrameHandler)))) {
        g_features |= kFeatureHoldMove;
    }

    const hook::Site orientationSite = {"Camera angles", game::kCameraOrientation, game::kCameraOrientationBytes,
                                        sizeof(game::kCameraOrientationBytes),
                                        sizeof(game::kCameraOrientationBytes)};
    if (hook::IsActive(hook::Detour(orientationSite, reinterpret_cast<void*>(CameraOrientationHook),
                                    reinterpret_cast<void**>(&g_originalCameraOrientation)))) {
        g_features |= kFeatureCameraAngles;
    }

    const hook::Site collisionSite = {"Camera distance", game::kCameraCollision, game::kCameraCollisionBytes,
                                      sizeof(game::kCameraCollisionBytes), sizeof(game::kCameraCollisionBytes)};
    if (hook::IsActive(hook::Detour(collisionSite, reinterpret_cast<void*>(CameraCollisionHook),
                                    reinterpret_cast<void**>(&g_originalCameraCollision)))) {
        g_features |= kFeatureCameraDistance;
    }

    if (cutaway::Install()) {
        g_features |= kFeatureCutaway | kFeatureInsideView | kFeatureRooms;
    }

    const auto state = [](uint32_t feature) { return (g_features & feature) != 0 ? "on" : "OFF"; };
    logfile::Write("Ready. Lua functions %s, hold-to-move %s, camera angles %s, camera distance %s, cutaway %s.",
                   state(kFeatureLua), state(kFeatureHoldMove), state(kFeatureCameraAngles),
                   state(kFeatureCameraDistance), state(kFeatureCutaway));
}

}  // namespace

extern "C" int WINAPI DllMain(void* module, uint32_t reason, void* /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        Attach(module);
    }
    return 1;
}
