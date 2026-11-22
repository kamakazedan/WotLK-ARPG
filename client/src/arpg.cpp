// WotLK-ARPG client library.
//
// What the Lua sandbox cannot do: move by holding the mouse button, and pin
// the camera. The library keeps no settings of its own; the addon pushes them
// in through the Lua functions registered at the bottom of this file, and
// they are dropped again whenever the UI reloads.

#include "game.h"
#include "hook.h"
#include "log.h"
#include "win.h"

#define ARPG_VERSION "0.2.0"

namespace {

constexpr float kRadiansPerDegree = 0.017453292f;

// Reported to the addon so it can say which parts are working.
enum Feature : uint32_t {
    kFeatureLua = 1,
    kFeatureHoldMove = 2,
    kFeatureCameraAngles = 4,
    kFeatureCameraDistance = 8,
};
uint32_t g_features = 0;

// ---------------------------------------------------------------------------
// Hold to move
//
// The addon binds the left mouse button to ARPG_HoldStart / ARPG_HoldStop.
// Those only set flags. The work happens in the frame handler, outside any
// addon code, using the client's own click handling: capture what is under
// the cursor, then act on it as a right click would. On terrain that is the
// client's own click-to-move, repeated while the button is held.
// ---------------------------------------------------------------------------

struct HoldMove {
    bool held;
    bool pressPending;  // a press not yet acted on (survives a release in the same frame)
    uint32_t lastTick;
    uint32_t intervalMs;
};
HoldMove g_hold = {false, false, 0, 100};

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
            game::PerformDefaultAction(worldFrame, game::kClickLeft);
        }
        return;
    }
    // A press acts on anything (move, attack, talk, loot). While held, only
    // moving repeats: nobody wants to re-open a vendor ten times a second.
    if (press || kind == game::kHitTerrain || kind == game::kHitWorldObject) {
        game::PerformDefaultAction(worldFrame, game::kClickRight);
    }
}

using FrameHandler = int (__cdecl*)(int, int, int, int);
FrameHandler g_originalFrameHandler = nullptr;

int __cdecl FrameHandlerHook(int a1, int a2, int a3, int a4) {
    HoldMoveTick();
    return g_originalFrameHandler(a1, a2, a3, a4);
}

// ---------------------------------------------------------------------------
// Camera lock
//
// The client works out the camera's angles and distance afresh every frame.
// Two hooks replace the results of those two steps, so nothing the client
// does in between (following the character, smoothing, mouse-look) can move
// the camera, and nothing is left changed when the lock is released.
// ---------------------------------------------------------------------------

struct CameraLock {
    bool locked;
    bool collide;
    float yaw;       // radians, 0 = looking north
    float pitch;     // radians, positive = looking down
    float distance;  // yards from the character
};
CameraLock g_camera = {false, false, 0.0f, 0.0f, 0.0f};

bool CameraIsLocked(void* camera) {
    return g_camera.locked && camera == game::ActiveCamera() &&
        (game::At<uint32_t>(camera, game::kCameraFlags) & game::kCameraFlagModelDriven) == 0;
}

using CameraOrientation = void (__thiscall*)(void* camera, void* target, float* yaw, float* pitch, float* roll);
CameraOrientation g_originalCameraOrientation = nullptr;

// __fastcall with an unused second parameter is how a free function receives
// a __thiscall call: `this` in ecx, the rest on the stack, callee cleans up.
void __fastcall CameraOrientationHook(void* camera, void* /*edx*/, void* target, float* yaw, float* pitch,
                                      float* roll) {
    g_originalCameraOrientation(camera, target, yaw, pitch, roll);
    if (target != nullptr && CameraIsLocked(camera)) {
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
    if (!CameraIsLocked(camera)) {
        return g_originalCameraCollision(camera, target, distance, height, offset, scale, fraction);
    }

    // The routine starts from the camera's wanted zoom and shortens it where
    // something is in the way. Lend it the locked distance for this one call,
    // and unless collision is wanted, set the flag that makes it return the
    // wanted distance untouched.
    float& zoom = game::At<float>(camera, game::kCameraZoom);
    float& zoomBlend = game::At<float>(camera, game::kCameraZoomBlend);
    uint32_t& flags = game::At<uint32_t>(camera, game::kCameraFlags);
    const float savedZoom = zoom;
    const float savedZoomBlend = zoomBlend;
    const uint32_t savedFlags = flags;

    zoom = g_camera.distance;
    zoomBlend = g_camera.distance;
    if (!g_camera.collide) {
        flags |= game::kCameraFlagNoCollision;
    }
    const uint32_t result = g_originalCameraCollision(camera, target, distance, height, offset, scale, fraction);
    flags = (flags & ~game::kCameraFlagNoCollision) | (savedFlags & game::kCameraFlagNoCollision);
    zoom = savedZoom;
    zoomBlend = savedZoomBlend;
    return result;
}

// ---------------------------------------------------------------------------
// Lua functions for the addon
// ---------------------------------------------------------------------------

float Clamp(float value, float low, float high) {
    return value < low ? low : (value > high ? high : value);
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
    if (game::lua_type(L, 1) == game::LUA_TNUMBER) {
        g_hold.intervalMs = static_cast<uint32_t>(Clamp(static_cast<float>(game::lua_tonumber(L, 1)), 10.0f, 2000.0f));
    }
    return 0;
}

// ARPG_SetCamera(yawDegrees, pitchDegrees, distance [, collide]) locks the
// camera; ARPG_SetCamera() with no numbers releases it.
int __cdecl LuaSetCamera(game::lua_State* L) {
    if (game::lua_gettop(L) < 3 || game::lua_type(L, 1) != game::LUA_TNUMBER ||
        game::lua_type(L, 2) != game::LUA_TNUMBER || game::lua_type(L, 3) != game::LUA_TNUMBER) {
        g_camera.locked = false;
        return 0;
    }
    g_camera.yaw = static_cast<float>(game::lua_tonumber(L, 1)) * kRadiansPerDegree;
    g_camera.pitch = Clamp(static_cast<float>(game::lua_tonumber(L, 2)), -89.0f, 89.0f) * kRadiansPerDegree;
    g_camera.distance = Clamp(static_cast<float>(game::lua_tonumber(L, 3)), 1.0f, 200.0f);
    g_camera.collide = game::lua_gettop(L) >= 4 && game::lua_toboolean(L, 4) != 0;
    g_camera.locked = true;
    return 0;
}

// Runs each time the client has finished registering its own Lua functions,
// which is once per UI load. A fresh UI starts from a clean slate: the addon
// has to ask again for anything it wants.
void __cdecl OnLuaStateCreated() {
    g_hold.held = false;
    g_hold.pressPending = false;
    g_camera.locked = false;

    game::RegisterLuaFunction("ARPG_ClientVersion", LuaClientVersion);
    game::RegisterLuaFunction("ARPG_HoldStart", LuaHoldStart);
    game::RegisterLuaFunction("ARPG_HoldStop", LuaHoldStop);
    game::RegisterLuaFunction("ARPG_SetHoldInterval", LuaSetHoldInterval);
    game::RegisterLuaFunction("ARPG_SetCamera", LuaSetCamera);
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

    const auto state = [](uint32_t feature) { return (g_features & feature) != 0 ? "on" : "OFF"; };
    logfile::Write("Ready. Lua functions %s, hold-to-move %s, camera angles %s, camera distance %s.",
                   state(kFeatureLua), state(kFeatureHoldMove), state(kFeatureCameraAngles),
                   state(kFeatureCameraDistance));
}

}  // namespace

extern "C" int WINAPI DllMain(void* module, uint32_t reason, void* /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        Attach(module);
    }
    return 1;
}
