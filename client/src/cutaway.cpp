// The cutaway: seeing the character under a roof.
//
// A camera fixed high above the character is fine in the open and useless
// under a roof: the client draws the roof. What an ARPG shows there is the
// room, looked into from above, and nothing else.
//
// Indoors: the inside view.
//
//   The client can already draw that picture. When it takes the camera to be
//   inside a building it draws from the camera's room outwards through the
//   doorways, and the walls and ceilings of rooms are made to be seen from
//   inside only, so a camera that is really above the roof looks straight
//   through the roof and the near walls at the room. It only happens by
//   accident, where the building has no separate roof for the client to find
//   under the camera. Here it is made to happen wherever the character is in
//   a room:
//
//   1. The room. The client finds "the building the camera is in" with a
//      line straight down from the camera. For the length of that one
//      question the camera's place is taken by the character's. The walk
//      through the doorways still judges each doorway from where the camera
//      really is, which leaves out the rooms between camera and character.
//   2. Nothing else. After the walk the doorways and windows it found to the
//      outdoor world are forgotten, so no street, terrain or sky is drawn;
//      the screen is cleared to black; and fog, which would fade far walls
//      to the colour of a sky that is not there, fades them to black, from
//      the character's distance on.
//   3. The rooms' contents. The client shows the furniture and people of a
//      room next door only as far as its doorway frames them. Each room the
//      walk reached is given the whole screen as its frame.
//   4. The cut. Beams, galleries and the floor of the storey above are solid
//      and face the camera. Everything above a level plane over the
//      character's feet is clipped away while the world is drawn. The plane
//      takes the place of the near side of the view: the projection matrix is
//      rebuilt so that depth starts at the plane instead of at the camera
//      (Eric Lengyel's oblique near-plane clipping). x, y and w of every
//      vertex come out as before, so nothing moves on screen; only depth
//      changes. The client decides what is in view from the corners of the
//      view, and the corners of a view cut this way are no use to it, so the
//      changed projection goes in only once that is decided, just before
//      drawing.
//   5. The cursor. Its ray starts at the camera and would stop at the roof
//      and the near wall, which are not on screen. It is picked up again at
//      the cut and then past every surface it meets until it is inside a
//      room that is drawn.
//
// Where the client's lookup finds no room although the client's own indoors
// test says the character is in one, and outdoors when asked: the cut alone.
//
//   The world is drawn as usual and cut at the plane. Every indoor group in
//   view is "visited", which is the client's own way of queueing a room, so
//   that buildings sliced open are not empty. This is the rougher picture:
//   whatever is taller than the cut loses its top, and buildings that are
//   only a shell show it.
//
// Where the cut is made:
//   - Under a roof lower than the camera: at a set height over the
//     character's feet. Under a ceiling lower than that, just under the
//     ceiling. In a tall room, just under whatever is in the way.
//   - Under a roof higher than the camera, seen from inside: not at all.
//   - Under the open sky: only if asked, while part of a building stands
//     between the camera and the character, and then just under that.
// The plane slides down from the camera when a cut begins and back up when
// it ends. In between it comes down at once and goes back up only after a
// pause longer than a jump, so that neither jumping nor walking under a row
// of beams makes it bob.
//
// All of this is switched by the addon through ARPG_SetCutaway and is off
// until it asks.

#include "cutaway.h"

#include "hook.h"
#include "log.h"
#include "win.h"

namespace cutaway {
namespace {

using game::At;
using game::Matrix;
using game::Vec3;

constexpr float kMinHeight = 2.0f;
constexpr float kMaxHeight = 30.0f;
constexpr float kHeadroom = 3.0f;       // yards over the feet the cut keeps clear of, so the character stays whole
constexpr float kEyeMargin = 1.0f;      // yards under the camera where a cut starts and ends
constexpr float kSlideSpeed = 60.0f;    // yards per second the plane comes down, and moves when a cut begins or ends
constexpr float kRiseSpeed = 10.0f;     // yards per second it goes back up while a cut continues
constexpr float kChest = 1.5f;          // yards over the feet: where the line of sight to the camera starts,
                                        // so that a waist-high wall or a kerb is not "in the way"
constexpr float kWaist = 1.0f;          // yards over the feet: where the look straight up starts
constexpr float kSky = 500.0f;          // yards up that look goes
constexpr float kBlockMargin = 0.3f;    // yards under whatever is in the way the cut is made
constexpr float kTallRoom = 3.0f;       // a ceiling more than this over the set height makes a room a tall one
constexpr uint32_t kBlockDelayMs = 150;  // in the open, a block has to last this long to count (a post going by does not)
constexpr uint32_t kClearDelayMs = 400;  // and the view has to stay clear this long before the cut ends
constexpr uint32_t kRiseDelayMs = 1000;  // the plane goes back up only once it could have for this long: more than a jump
constexpr uint32_t kMaxRefusals = 3;     // times the device may turn the cut projection into something else
constexpr int kMaxUnseen = 6;            // surfaces not on screen a click may pass on its way into a drawn room
constexpr float kProbe = 200.0f;         // yards up and down a point is looked at to tell which room it is in
constexpr float kPast = 0.05f;           // yards past a surface the cursor's ray is picked up again
constexpr uint32_t kBlack = 0xFF000000;
constexpr int kMaxGroups = 1024;         // per building: far more than one has; ends a walk over a broken list

bool g_installed = false;

struct Settings {
    bool enabled;   // indoors, the inside view
    float height;   // yards over the character's feet at which the world is cut indoors
    bool outdoors;  // outdoors, cut the world while a building stands between camera and character
};
Settings g_settings = {false, 4.0f, false};

struct State {
    bool cutting;  // a cut is in progress, sliding included
    bool active;   // ... and this frame's scene update has prepared this frame's draw for it
    float planeZ;  // world height of the cut
    uint32_t lastTick;
    Vec3 feet;
    Vec3 eye;

    bool indoors;       // the character is in a building: by the scene's own lookup or, failing that, by the
                        // client's indoors test
    bool interior;      // the scene's lookup found the room, and this frame is drawn from it: the inside view
    bool fromOutside;   // ... with the camera above the room's roof, looking in from the dark
    float away;         // yards from the camera to the character
    float overhead;     // yards over the feet of what is overhead, 0 if nothing was found (looked for indoors only)
    float between;      // yards over the feet of the lowest thing between camera and character, 0 if nothing
                        // (not looked for in the inside view, where walls do not hide what is behind them)
    bool blocked;       // in the open, with the view of the character blocked for long enough
    bool blockPending;
    uint32_t blockedSince;
    uint32_t lastBlockedTick;
    float blockZ;       // height of the lowest thing in the way
    bool rising;        // the plane could go up
    uint32_t risingSince;

    bool givenUp;       // the device does not keep the cut projection as expected
    uint32_t refusals;
};
State g_state = {};

enum SkipReason : int {
    kSkipNoDevice,
    kSkipProjection,
    kSkipView,
    kSkipCharacter,
    kSkipPlane,
    kSkipReasons,
};

struct Stats {
    uint32_t frames;         // scene updates seen
    uint32_t framesCut;      // draws made with the cut projection
    uint32_t framesSkipped;  // draws where a cut was due but the device's matrices were not as expected
    uint32_t framesInside;   // draws made as the inside view
    uint32_t rooms;          // rooms drawn in the last frame of the inside view, or queued in the last cut one
    bool logged[kSkipReasons];
    bool loggedRefusal;
};
Stats g_stats = {};

// The rooms of the last frame drawn as the inside view: for the cursor.
struct Drawn {
    void* building;  // compared, never followed: it may be gone by the time the cursor asks
    uint32_t groups[kMaxGroups / 32];
};
Drawn g_drawn = {};

float Abs(float value) {
    return value < 0.0f ? -value : value;
}

float Min(float a, float b) {
    return a < b ? a : b;
}

float Max(float a, float b) {
    return a > b ? a : b;
}

int Thousandths(float value) {
    return static_cast<int>(value * 1000.0f);
}

// There is no C runtime here. Newton's method; exact enough after a few
// rounds for the distances this is used on.
float Sqrt(float value) {
    if (!(value > 0.0f)) {
        return 0.0f;
    }
    float root = value > 1.0f ? value * 0.5f : 1.0f;
    for (int i = 0; i < 20; ++i) {
        root = 0.5f * (root + value / root);
    }
    return root;
}

// ---------------------------------------------------------------------------
// When and where to cut
// ---------------------------------------------------------------------------

// The height at which the line from `from` to `to` first meets something
// solid. The result is only believed if the hit lies where the routine's own
// "how far along" says it does, measured from either end.
bool HitHeight(const Vec3& from, const Vec3& to, float* height) {
    Vec3 hit = {0.0f, 0.0f, 0.0f};
    float fraction = 1.0f;
    if (!game::IntersectWorld(from, to, &hit, &fraction, game::kIntersectBuildings)) {
        return false;
    }
    if (!(fraction >= 0.0f && fraction <= 1.0f)) {
        return false;
    }
    const float fromStart = from.z + (to.z - from.z) * fraction;
    const float fromEnd = to.z + (from.z - to.z) * fraction;
    if (Abs(hit.z - fromStart) > 0.5f && Abs(hit.z - fromEnd) > 0.5f) {
        return false;
    }
    *height = hit.z;
    return true;
}

// Whether part of a building lies on the line between two points, and the
// height of the lowest such part. The line is tried in both directions, so
// it does not matter from which end the client counts "first".
bool LowestBetween(const Vec3& low, const Vec3& high, float* lowest) {
    float up = 0.0f;
    float down = 0.0f;
    const bool hitUp = HitHeight(low, high, &up);
    const bool hitDown = HitHeight(high, low, &down);
    if (!hitUp && !hitDown) {
        return false;
    }
    *lowest = hitUp && hitDown ? Min(up, down) : (hitUp ? up : down);
    return true;
}

// Once per frame, from the scene update: decides whether this frame is cut
// and at what height. `inRoom`: the scene's lookup found the room the
// character is in, and the frame will be drawn from it.
void UpdateForFrame(void* player, const Vec3& feet, const Vec3& eye, bool inRoom) {
    const uint32_t now = GetTickCount();
    const float elapsed = Min(static_cast<float>(now - g_state.lastTick) * 0.001f, 0.1f);
    g_state.lastTick = now;
    g_state.feet = feet;
    g_state.eye = eye;
    g_state.indoors = inRoom || !game::UnitIsOutdoors(player);
    g_state.interior = inRoom;

    // What stands between the camera and the character, and what is overhead.
    // In the inside view a wall between camera and character is not drawn
    // from the camera's side, so there the line of sight is not asked about.
    const Vec3 chest = {feet.x, feet.y, feet.z + kChest};
    const Vec3 waist = {feet.x, feet.y, feet.z + kWaist};
    const Vec3 sky = {feet.x, feet.y, feet.z + kSky};
    float sightZ = 0.0f;
    float roofZ = 0.0f;
    const bool sight = !inRoom && (g_state.indoors || g_settings.outdoors) && LowestBetween(chest, eye, &sightZ);
    const bool roof = g_state.indoors && LowestBetween(waist, sky, &roofZ);
    g_state.between = sight ? sightZ - feet.z : 0.0f;
    g_state.overhead = roof ? roofZ - feet.z : 0.0f;

    // Enclosed: indoors under a roof lower than the camera. Indoors with no
    // roof found at all counts too: every indoor place has one, so the look
    // upwards has failed and is no ground for leaving the roof on.
    const bool enclosed = g_state.indoors && !(roof && roofZ >= eye.z);
    g_state.fromOutside = inRoom && enclosed;
    const float toChest[3] = {chest.x - eye.x, chest.y - eye.y, chest.z - eye.z};
    g_state.away = Sqrt(toChest[0] * toChest[0] + toChest[1] * toChest[1] + toChest[2] * toChest[2]);

    if (g_state.givenUp) {
        // The device does not take the cut. The inside view needs none.
        g_state.cutting = false;
        g_state.blocked = false;
        g_state.blockPending = false;
        return;
    }

    // The cut keeps clear of the character, and a jump takes the cut up with
    // it only for as far as that needs.
    const float room = feet.z + g_settings.height;
    float limit = feet.z + Min(g_settings.height, kHeadroom);

    bool wanted = false;
    float target = 0.0f;
    if (enclosed) {
        g_state.blocked = false;
        g_state.blockPending = false;
        wanted = true;
        target = room;
        if (roof && roofZ - kBlockMargin < room) {
            // A ceiling lower than the cut asked for. Cut above it, it would
            // lie over the room like a lid; so under it, even where that
            // leaves less room over the character than usual.
            target = roofZ - kBlockMargin;
            limit = Min(limit, Max(target, feet.z + kMinHeight));
        } else if (sight || roof) {
            const float lowest = sight && roof ? Min(sightZ, roofZ) : (sight ? sightZ : roofZ);
            if (lowest > room + kTallRoom) {
                target = lowest - kBlockMargin;  // a tall room: only as much as is in the way
            }
        }
    } else if (inRoom) {
        // A hall higher than the camera, seen from inside: nothing to cut.
        g_state.blocked = false;
        g_state.blockPending = false;
    } else {
        // Open sky (or a hall higher than the camera that the scene's
        // lookup did not find).
        if (sight && g_settings.outdoors) {
            g_state.blockZ = sightZ;
            g_state.lastBlockedTick = now;
            if (!g_state.blockPending) {
                g_state.blockPending = true;
                g_state.blockedSince = now;
            }
            if (now - g_state.blockedSince >= kBlockDelayMs) {
                g_state.blocked = true;
            }
        } else {
            g_state.blockPending = false;
            if (!g_settings.outdoors || now - g_state.lastBlockedTick >= kClearDelayMs) {
                g_state.blocked = false;
            }
        }
        wanted = g_state.blocked;
        target = g_state.blockZ - kBlockMargin;
    }

    const float top = eye.z - kEyeMargin;
    if (wanted) {
        target = Max(target, limit);
        if (target >= top) {
            wanted = false;  // the camera is too low for there to be anything to cut
        }
    }

    if (wanted) {
        if (!g_state.cutting) {
            g_state.cutting = true;
            g_state.rising = false;
            g_state.planeZ = top;
        }
        float plane = Min(g_state.planeZ, top);
        if (target <= plane) {
            g_state.rising = false;
            plane = Max(target, plane - kSlideSpeed * elapsed);
        } else {
            if (!g_state.rising) {
                g_state.rising = true;
                g_state.risingSince = now;
            }
            if (now - g_state.risingSince >= kRiseDelayMs) {
                plane = Min(target, plane + kRiseSpeed * elapsed);
            }
        }
        g_state.planeZ = Max(plane, limit);  // whatever else, the character stays whole
    } else if (g_state.cutting) {
        g_state.planeZ += kSlideSpeed * elapsed;
        if (g_state.planeZ >= top) {
            g_state.cutting = false;
        }
    }
    g_state.active = g_state.cutting;
}

// ---------------------------------------------------------------------------
// The room the scene is drawn from
// ---------------------------------------------------------------------------

using Plain = void (__cdecl*)();
Plain g_originalLocateCamera = nullptr;

void __cdecl LocateCameraHook() {
    ++g_stats.frames;
    g_state.active = false;
    g_state.interior = false;
    g_state.fromOutside = false;
    void* player = nullptr;
    if (!g_installed || !g_settings.enabled || !game::InWorld() || (player = game::PlayerObject()) == nullptr) {
        g_state.cutting = false;
        g_state.indoors = false;
        g_state.blocked = false;
        g_state.blockPending = false;
        g_state.overhead = 0.0f;
        g_state.between = 0.0f;
        g_originalLocateCamera();
        return;
    }

    // The client's routine asks which building lies under the camera, by a
    // line straight down from kSceneCameraPosition, which it reads once at
    // its start. Asked from the character's chest instead, it answers with
    // the room the character is in, or with "none" under the open sky, even
    // while the camera itself hangs inside somebody's attic.
    Vec3 feet;
    game::ObjectPosition(player, &feet);
    Vec3* camera = reinterpret_cast<Vec3*>(game::kSceneCameraPosition);
    const Vec3 eye = *camera;
    *camera = {feet.x, feet.y, feet.z + kChest};
    g_originalLocateCamera();
    *camera = eye;
    const bool inRoom = *reinterpret_cast<void* const*>(game::kSceneCameraBuilding) != nullptr;

    UpdateForFrame(player, feet, eye, inRoom);
}

// ---------------------------------------------------------------------------
// Nothing else: no outdoor world, a black screen behind, black fog
// ---------------------------------------------------------------------------

using WalkRooms = void (__thiscall*)(uint8_t* building, void* groups);
WalkRooms g_originalWalkRooms = nullptr;

// The client's walk from the camera's room through the doorways. Whatever
// doorways and windows to the outdoor world it has come across are forgotten
// again: the client then draws no outdoor world and no sky.
void __fastcall WalkRoomsHook(uint8_t* building, void* /*edx*/, void* groups) {
    g_originalWalkRooms(building, groups);
    if (g_state.interior) {
        *reinterpret_cast<game::OutdoorWindow*>(game::kSceneSkyWindow) = game::kNoOutdoorWindow;
        *reinterpret_cast<game::OutdoorWindow*>(game::kSceneOutdoorWindow) = game::kNoOutdoorWindow;
        ++g_stats.framesInside;
    }
}

using SceneClear = void (__cdecl*)(uint32_t what, uint32_t colour);
SceneClear g_originalSceneClear = nullptr;

void __cdecl SceneClearHook(uint32_t what, uint32_t colour) {
    g_originalSceneClear(what, g_state.interior ? kBlack : colour);
}

using SetFog = void (__cdecl*)(float start, float end, float density, const uint32_t* colour);
SetFog g_originalSetFog = nullptr;

// Seen from above the roof, the room stands in the dark. Its fog would fade
// the far end of it to the colour of the sky; it fades to black instead. And
// it would start at the camera, which is a long way off: it starts at the
// character, so that the room around the character is as clear as it would
// be for a camera standing in it.
void __cdecl SetFogHook(float start, float end, float density, const uint32_t* colour) {
    if (g_state.fromOutside && g_settings.enabled) {
        static const uint32_t black = kBlack;
        g_originalSetFog(start + g_state.away, end + g_state.away, density, &black);
        return;
    }
    g_originalSetFog(start, end, density, colour);
}

// ---------------------------------------------------------------------------
// The rooms
// ---------------------------------------------------------------------------

// A list link is followed while it is a real pointer: the client ends its
// lists with null or with an odd value.
bool IsLink(uintptr_t value) {
    return value != 0 && (value & 1) == 0;
}

// The placed group the visit routine will look up for this index, or null if
// the building has no such slot. Mirrors the routine's own lookup.
void* PlacedGroupAt(uint8_t* building, uint32_t index) {
    const int32_t mode = At<int32_t>(building, game::kBuildingGroupArrayMode);
    if (mode == -1) {
        if (index >= At<uint32_t>(building, game::kBuildingGroupArrayCount)) {
            return nullptr;
        }
        void** groups = At<void**>(building, game::kBuildingGroupArrayData);
        return groups != nullptr ? groups[index] : nullptr;
    }
    if (mode < 0 || index >= static_cast<uint32_t>(mode) || index >= 4) {
        return nullptr;
    }
    return At<void*>(building, game::kBuildingGroupArray + index * sizeof(void*));
}

constexpr int kMaxBuildings = 4096;  // far more than a scene holds; ends a walk over a broken list

void ShowRoomsOf(uint8_t* building) {
    if ((At<uint32_t>(building, game::kBuildingFlags) & game::kBuildingFlagGroupsPlaced) == 0) {
        return;
    }
    uint8_t* model = At<uint8_t*>(building, game::kBuildingModel);
    if (model == nullptr || At<uint32_t>(model, game::kModelLoaded) == 0) {
        return;
    }
    const game::Box& bounds = At<game::Box>(building, game::kBuildingBounds);
    if (bounds.low.z >= g_state.planeZ || game::BoxOutsideView(&bounds)) {
        return;
    }
    const uint32_t groupCount = At<uint32_t>(model, game::kModelGroupCount);
    const uint8_t* groupInfo = At<const uint8_t*>(model, game::kModelGroupInfo);
    void** groupData = &At<void*>(model, game::kModelGroups);
    if (groupInfo == nullptr) {
        return;
    }

    const int32_t linkOffset = At<int32_t>(building, game::kBuildingGroupLinkOffset);
    uintptr_t link = At<uintptr_t>(building, game::kBuildingGroupLinks);
    for (int guard = 0; IsLink(link) && guard < kMaxGroups; ++guard) {
        uint8_t* placed = At<uint8_t*>(reinterpret_cast<void*>(link), game::kGroupLinkGroup);
        link = At<uintptr_t>(reinterpret_cast<void*>(link), static_cast<size_t>(linkOffset) + sizeof(void*));
        if (placed == nullptr) {
            continue;
        }
        const uint32_t index = At<uint32_t>(placed, game::kPlacedGroupIndex);
        if (index >= groupCount || PlacedGroupAt(building, index) != placed) {
            continue;
        }
        const uint32_t flags = *reinterpret_cast<const uint32_t*>(groupInfo + index * game::kModelGroupInfoSize);
        if ((flags & (game::kGroupFlagOutdoor | game::kGroupFlagAlwaysOutdoor)) != 0) {
            continue;  // the outdoor pass has these
        }
        uint8_t* data = static_cast<uint8_t*>(groupData[index]);
        if (data == nullptr || (At<uint32_t>(data, game::kGroupDataLoadFlags) & game::kGroupDataLoaded) == 0) {
            continue;  // still loading
        }
        const game::Box& box = At<game::Box>(placed, game::kPlacedGroupBounds);
        if (box.low.z >= g_state.planeZ || game::BoxOutsideView(&box)) {
            continue;  // wholly above the cut, or off screen
        }
        *reinterpret_cast<uint32_t*>(game::kSceneInteriorPath) = (flags & game::kGroupFlagOutdoorLit) != 0 ? 0 : 1;
        game::VisitBuildingGroup(index, building);
        ++g_stats.rooms;
    }
}

void ShowRooms() {
    g_stats.rooms = 0;
    if (*reinterpret_cast<const int32_t*>(game::kSceneFrustumLevel) != 0) {
        return;  // not the whole-screen view this is meant to run under
    }
    const uint32_t interiorPath = *reinterpret_cast<const uint32_t*>(game::kSceneInteriorPath);
    const int32_t linkOffset = *reinterpret_cast<const int32_t*>(game::kPlacedBuildingsLinkOffset);
    uintptr_t building = *reinterpret_cast<const uintptr_t*>(game::kPlacedBuildingsHead);
    for (int guard = 0; IsLink(building) && guard < kMaxBuildings; ++guard) {
        uint8_t* current = reinterpret_cast<uint8_t*>(building);
        building = At<uintptr_t>(current, static_cast<size_t>(linkOffset) + sizeof(void*));
        ShowRoomsOf(current);
    }
    *reinterpret_cast<uint32_t*>(game::kSceneInteriorPath) = interiorPath;
}

// The inside view: the rooms the client's walk has reached, and no others.
// Each is visited once more under the whole-screen view, so that everything
// in it is drawn and not only what its doorway frames; a group that is
// queued already is not queued twice. Which rooms these are is kept for the
// cursor.
void ShowDrawnRooms() {
    g_stats.rooms = 0;
    g_drawn = {};
    uint8_t* building = *reinterpret_cast<uint8_t* const*>(game::kSceneCameraBuilding);
    if (building == nullptr || *reinterpret_cast<const int32_t*>(game::kSceneFrustumLevel) != 0 ||
        (At<uint32_t>(building, game::kBuildingFlags) & game::kBuildingFlagGroupsPlaced) == 0) {
        return;
    }
    uint8_t* model = At<uint8_t*>(building, game::kBuildingModel);
    if (model == nullptr || At<uint32_t>(model, game::kModelLoaded) == 0) {
        return;
    }
    const uint32_t groupCount = At<uint32_t>(model, game::kModelGroupCount);
    void** groupData = &At<void*>(model, game::kModelGroups);
    g_drawn.building = building;

    // Leave the groups marked as the client's walk marked them.
    const uint32_t interiorPath = *reinterpret_cast<const uint32_t*>(game::kSceneInteriorPath);
    *reinterpret_cast<uint32_t*>(game::kSceneInteriorPath) = 0;
    const int32_t linkOffset = At<int32_t>(building, game::kBuildingGroupLinkOffset);
    uintptr_t link = At<uintptr_t>(building, game::kBuildingGroupLinks);
    for (int guard = 0; IsLink(link) && guard < kMaxGroups; ++guard) {
        uint8_t* placed = At<uint8_t*>(reinterpret_cast<void*>(link), game::kGroupLinkGroup);
        link = At<uintptr_t>(reinterpret_cast<void*>(link), static_cast<size_t>(linkOffset) + sizeof(void*));
        if (placed == nullptr || At<uint32_t>(placed, game::kPlacedGroupQueued) == 0) {
            continue;  // not reached by the walk
        }
        const uint32_t index = At<uint32_t>(placed, game::kPlacedGroupIndex);
        if (index >= groupCount || PlacedGroupAt(building, index) != placed) {
            continue;
        }
        if (index < static_cast<uint32_t>(kMaxGroups)) {
            g_drawn.groups[index / 32] |= 1u << (index % 32);
        }
        ++g_stats.rooms;
        uint8_t* data = static_cast<uint8_t*>(groupData[index]);
        if (data != nullptr && (At<uint32_t>(data, game::kGroupDataLoadFlags) & game::kGroupDataLoaded) != 0) {
            game::VisitBuildingGroup(index, building);
        }
    }
    *reinterpret_cast<uint32_t*>(game::kSceneInteriorPath) = interiorPath;
}

Plain g_originalFinishVisits = nullptr;

void __cdecl FinishVisitsHook() {
    if (g_state.interior) {
        ShowDrawnRooms();
    } else if (g_state.active) {
        ShowRooms();
    }
    g_originalFinishVisits();
}

// ---------------------------------------------------------------------------
// The cut
// ---------------------------------------------------------------------------

void Skip(SkipReason reason, int a, int b, int c) {
    ++g_stats.framesSkipped;
    if (!g_stats.logged[reason]) {
        g_stats.logged[reason] = true;
        static const char* const kWhy[kSkipReasons] = {
            "there is no graphics device",
            "the projection matrix is not the one the client builds for the world",
            "the view matrix is not a plain rotation",
            "the character is not where the matrices say the camera is looking",
            "the cut plane does not suit this view",
        };
        logfile::Write("Cutaway: no cut, because %s (%d, %d, %d). Logged once; ARPG_CutawayInfo counts the rest.",
                       kWhy[reason], a, b, c);
    }
}

bool IsZero(float value) {
    return Abs(value) < 1e-6f;
}

bool Close(float actual, float expected) {
    return Abs(actual - expected) <= 0.001f * Max(1.0f, Abs(expected));
}

// The shape documented in game.h.
bool IsWorldProjection(const Matrix& p) {
    return p.m[0][0] > 0.0f && IsZero(p.m[0][1]) && IsZero(p.m[0][2]) && IsZero(p.m[0][3]) &&
           IsZero(p.m[1][0]) && p.m[1][1] > 0.0f && IsZero(p.m[1][2]) && IsZero(p.m[1][3]) &&
           IsZero(p.m[2][0]) && IsZero(p.m[2][1]) && p.m[2][2] > 0.0f && Abs(p.m[2][3] - 1.0f) < 1e-4f &&
           IsZero(p.m[3][0]) && IsZero(p.m[3][1]) && p.m[3][2] < 0.0f && IsZero(p.m[3][3]);
}

float Dot(const float* a, const float* b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

bool IsRotation(const Matrix& v) {
    for (int i = 0; i < 3; ++i) {
        if (!IsZero(v.m[i][3])) {
            return false;
        }
        for (int j = i; j < 3; ++j) {
            if (Abs(Dot(v.m[i], v.m[j]) - (i == j ? 1.0f : 0.0f)) > 0.001f) {
                return false;
            }
        }
    }
    return Abs(v.m[3][3] - 1.0f) < 1e-4f;
}

float Sign(float value) {
    return value > 0.0f ? 1.0f : (value < 0.0f ? -1.0f : 0.0f);
}

// Replaces the device's projection with one that clips at the cut. Runs after
// the client has decided what is in view and before it draws; the client
// itself puts the device's projection back when the world is done.
void ApplyCut() {
    uint8_t* device = game::GxDevice();
    if (device == nullptr) {
        Skip(kSkipNoDevice, 0, 0, 0);
        return;
    }
    const Matrix projection = At<Matrix>(device, game::kGxDeviceProjection);
    const uint32_t level = At<uint32_t>(device, game::kGxDeviceViewLevel);
    if (!IsWorldProjection(projection)) {
        Skip(kSkipProjection, Thousandths(projection.m[2][2]), Thousandths(projection.m[2][3]),
             Thousandths(projection.m[3][3]));
        return;
    }
    if (level > 3) {
        Skip(kSkipView, static_cast<int>(level), 0, 0);
        return;
    }
    const Matrix view = At<Matrix>(device, game::kGxDeviceViewStack + level * sizeof(Matrix));
    if (!IsRotation(view)) {
        Skip(kSkipView, Thousandths(Dot(view.m[0], view.m[0])), Thousandths(Dot(view.m[1], view.m[1])),
             Thousandths(Dot(view.m[2], view.m[2])));
        return;
    }

    // A point p of the world is (p - eye) * view in view space, whichever
    // way the client splits that between the view matrix and its own
    // camera-relative coordinates. The character is on screen in front of
    // the camera; if these matrices put it anywhere else, they do not mean
    // what this code takes them to mean, and it keeps its hands off.
    const Vec3& eye = g_state.eye;
    const float toFeet[3] = {g_state.feet.x - eye.x, g_state.feet.y - eye.y, g_state.feet.z - eye.z};
    float feetView[3];
    for (int j = 0; j < 3; ++j) {
        feetView[j] = toFeet[0] * view.m[0][j] + toFeet[1] * view.m[1][j] + toFeet[2] * view.m[2][j];
    }
    const float depth = feetView[2];
    if (!(depth > 0.5f && Abs(feetView[0] * projection.m[0][0]) <= depth &&
          Abs(feetView[1] * projection.m[1][1]) <= depth)) {
        Skip(kSkipCharacter, Thousandths(feetView[0]), Thousandths(feetView[1]), Thousandths(depth));
        return;
    }

    // The plane "below planeZ", as (normal, offset) in view space, positive
    // on the side that stays: world normal (0, 0, -1) rotated into view
    // space, and the camera's height above the plane as the offset.
    const float plane[4] = {-view.m[2][0], -view.m[2][1], -view.m[2][2], g_state.planeZ - eye.z};

    // Lengyel: scale the plane so that the view's far corner on the plane's
    // side keeps its place at the far end of the depth range.
    const float corner[4] = {Sign(plane[0]) / projection.m[0][0], Sign(plane[1]) / projection.m[1][1], 1.0f,
                             (1.0f - projection.m[2][2]) / projection.m[3][2]};
    const float reach = plane[0] * corner[0] + plane[1] * corner[1] + plane[2] * corner[2] + plane[3] * corner[3];
    if (!(plane[3] < 0.0f && reach > 0.001f)) {
        Skip(kSkipPlane, Thousandths(plane[3]), Thousandths(reach), 0);
        return;
    }
    float scale = 2.0f / reach;
    // The camera has to look down at the plane to some degree. And the
    // device divides by (m[2][2] - 1) when it takes the matrix, which is
    // scale * plane[2] - 2 here: stay clear of zero.
    if (!(scale * plane[2] > 0.05f)) {
        Skip(kSkipPlane, Thousandths(plane[2]), Thousandths(scale), 1);
        return;
    }
    if (Abs(scale * plane[2] - 2.0f) < 0.02f) {
        scale *= 0.97f;  // costs a sliver of depth range, nothing else
    }

    // Depth is plane distance times scale, less w. The device will halve the
    // last two of these four on its way to Direct3D's depth range and leave
    // the first two alone (game.h), so those go in already halved.
    Matrix cut = projection;
    cut.m[0][2] = 0.5f * scale * plane[0];
    cut.m[1][2] = 0.5f * scale * plane[1];
    cut.m[2][2] = scale * plane[2] - 1.0f;
    cut.m[3][2] = scale * plane[3];
    game::SetProjection(device, &cut);

    // Did the device make of it what Direct3D needs: depth = half the scaled
    // plane distance, w untouched?
    const Matrix& hardware = At<Matrix>(device, game::kGxDeviceHardwareProjection);
    const float half = 0.5f * scale;
    if (!(Close(hardware.m[0][2], half * plane[0]) && Close(hardware.m[1][2], half * plane[1]) &&
          Close(hardware.m[2][2], half * plane[2]) && Close(hardware.m[3][2], half * plane[3]) &&
          Close(hardware.m[2][3], 1.0f) && IsZero(hardware.m[3][3]))) {
        if (!g_stats.loggedRefusal) {
            g_stats.loggedRefusal = true;
            logfile::Write("Cutaway: the graphics device turned the cut projection into something unexpected "
                           "(depth column %d %d %d %d, wanted %d %d %d %d, in thousandths). "
                           "This needs the Direct3D 9 renderer. No cut.",
                           Thousandths(hardware.m[0][2]), Thousandths(hardware.m[1][2]),
                           Thousandths(hardware.m[2][2]), Thousandths(hardware.m[3][2]),
                           Thousandths(half * plane[0]), Thousandths(half * plane[1]),
                           Thousandths(half * plane[2]), Thousandths(half * plane[3]));
        }
        game::SetProjection(device, &projection);
        game::UpdateShaderProjection();
        ++g_stats.framesSkipped;
        if (++g_state.refusals >= kMaxRefusals) {
            g_state.givenUp = true;
            g_state.cutting = false;
        }
        return;
    }
    game::UpdateShaderProjection();
    ++g_stats.framesCut;
}

Plain g_originalBeforeDrawing = nullptr;

void __cdecl BeforeDrawingHook() {
    g_originalBeforeDrawing();
    if (g_state.active) {
        g_state.active = false;  // one scene update prepares one draw
        ApplyCut();
    }
}

// ---------------------------------------------------------------------------
// The cursor
// ---------------------------------------------------------------------------

bool IsDrawnRoom(void* building, uint32_t group) {
    return building != nullptr && building == g_drawn.building && group < static_cast<uint32_t>(kMaxGroups) &&
           (g_drawn.groups[group / 32] >> (group % 32) & 1u) != 0;
}

// The room whose surface the line from one point to another meets first.
bool RoomOnLine(const Vec3& from, const Vec3& to, void** building, uint32_t* group) {
    void* buildings[2] = {nullptr, nullptr};
    uint32_t groups[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
    if (!game::FindGroupOnLine(from, to, buildings, groups) || buildings[0] == nullptr) {
        return false;
    }
    *building = buildings[0];
    *group = groups[0];
    return true;
}

// Whether a point is in a room that is on screen: the nearest building
// surface below it and the nearest above it both belong to rooms that were
// drawn. Above a ceiling, between the walls of two rooms, in a room that
// was not drawn or out in the street, one of the two is something else.
bool InDrawnRoom(const Vec3& point) {
    const Vec3 below = {point.x, point.y, point.z - kProbe};
    const Vec3 above = {point.x, point.y, point.z + kProbe};
    void* building = nullptr;
    uint32_t group = 0;
    return RoomOnLine(point, below, &building, &group) && IsDrawnRoom(building, group) &&
           RoomOnLine(point, above, &building, &group) && IsDrawnRoom(building, group);
}

// In the inside view the roof, the wall on the camera's side, the house next
// door and the hill outside are not on screen, but they would still stop the
// cursor's ray. The ray is picked up again past each thing it meets, until
// it starts in a room that is drawn. If it never gets into one it is left
// as it was.
void EnterDrawnRoom(Vec3* start, const Vec3& end) {
    if (g_drawn.building == nullptr ||
        (*reinterpret_cast<const uint32_t*>(game::kWorldSwitches) & game::kWorldSwitchBuildings) == 0) {
        return;
    }
    Vec3 point = *start;
    for (int unseen = 0; unseen <= kMaxUnseen; ++unseen) {
        if (InDrawnRoom(point)) {
            *start = point;
            return;
        }
        const float span = Max(Max(Abs(end.x - point.x), Abs(end.y - point.y)), Abs(end.z - point.z));
        Vec3 hit = {0.0f, 0.0f, 0.0f};
        float fraction = 1.0f;
        if (!(span > 1.0f) || !game::IntersectWorld(point, end, &hit, &fraction, game::kIntersectClick) ||
            !(fraction >= 0.0f)) {
            return;
        }
        const float along = fraction + kPast / span;
        if (!(along < 1.0f)) {
            return;
        }
        point = {point.x + (end.x - point.x) * along, point.y + (end.y - point.y) * along,
                 point.z + (end.z - point.z) * along};
    }
}

using CursorRay = int (__thiscall*)(uint8_t* worldFrame, float x, float y, Vec3* start, Vec3* end);
CursorRay g_originalCursorRay = nullptr;

int __fastcall CursorRayHook(uint8_t* worldFrame, void* /*edx*/, float x, float y, Vec3* start, Vec3* end) {
    const int found = g_originalCursorRay(worldFrame, x, y, start, end);
    if (found == 0 || start == nullptr || end == nullptr || !g_installed || !g_settings.enabled) {
        return found;
    }
    if (g_state.cutting && start->z > g_state.planeZ && end->z < g_state.planeZ) {
        // Start where the ray comes through the cut: what is above it is not
        // on screen and must not catch the click.
        const float along = (start->z - g_state.planeZ) / (start->z - end->z);
        start->x += (end->x - start->x) * along;
        start->y += (end->y - start->y) * along;
        start->z = g_state.planeZ;
    }
    if (g_state.interior) {
        EnterDrawnRoom(start, *end);
    }
    return found;
}

}  // namespace

bool Install() {
    const hook::Site locate = {"Cutaway: camera in building", game::kLocateCameraCall,
                               game::kLocateCameraCallBytes, sizeof(game::kLocateCameraCallBytes), 0};
    const hook::Site walk = {"Cutaway: walk through the rooms", game::kWalkRoomsCall, game::kWalkRoomsCallBytes,
                             sizeof(game::kWalkRoomsCallBytes), 0};
    const hook::Site visits = {"Cutaway: rooms", game::kFinishVisitsCall, game::kFinishVisitsCallBytes,
                               sizeof(game::kFinishVisitsCallBytes), 0};
    const hook::Site drawing = {"Cutaway: projection", game::kBeforeDrawingCall, game::kBeforeDrawingCallBytes,
                                sizeof(game::kBeforeDrawingCallBytes), 0};
    const hook::Site clear = {"Cutaway: background", game::kSceneClearCall, game::kSceneClearCallBytes,
                              sizeof(game::kSceneClearCallBytes), 0};
    const hook::Site ray = {"Cutaway: cursor ray", game::kCursorRayCall, game::kCursorRayCallBytes,
                            sizeof(game::kCursorRayCallBytes), 0};
    const hook::Site fog = {"Cutaway: fog", game::kSetFog, game::kSetFogBytes, sizeof(game::kSetFogBytes),
                            sizeof(game::kSetFogBytes)};

    // Each hook passes straight through while g_installed is false, so a
    // partial set does no harm; the cutaway only runs with all six.
    bool all = true;
    all &= hook::IsActive(hook::RedirectCall(locate, reinterpret_cast<void*>(LocateCameraHook),
                                             reinterpret_cast<void**>(&g_originalLocateCamera)));
    all &= hook::IsActive(hook::RedirectCall(walk, reinterpret_cast<void*>(WalkRoomsHook),
                                             reinterpret_cast<void**>(&g_originalWalkRooms)));
    all &= hook::IsActive(hook::RedirectCall(visits, reinterpret_cast<void*>(FinishVisitsHook),
                                             reinterpret_cast<void**>(&g_originalFinishVisits)));
    all &= hook::IsActive(hook::RedirectCall(drawing, reinterpret_cast<void*>(BeforeDrawingHook),
                                             reinterpret_cast<void**>(&g_originalBeforeDrawing)));
    all &= hook::IsActive(hook::RedirectCall(clear, reinterpret_cast<void*>(SceneClearHook),
                                             reinterpret_cast<void**>(&g_originalSceneClear)));
    all &= hook::IsActive(hook::RedirectCall(ray, reinterpret_cast<void*>(CursorRayHook),
                                             reinterpret_cast<void**>(&g_originalCursorRay)));
    g_installed = all;

    // The fog is a matter of looks: without this the far end of a room fades
    // to the fog's own colour instead of to black, and all else works.
    hook::Detour(fog, reinterpret_cast<void*>(SetFogHook), reinterpret_cast<void**>(&g_originalSetFog));
    return all;
}

void Reset() {
    g_settings.enabled = false;
    g_state = {};
    g_stats = {};
    g_drawn = {};
}

// ARPG_SetCutaway(height [, outdoors]): while the character is in a
// building, show the building from inside and nothing else, with whatever is
// more than `height` yards above the character's feet left out. With
// outdoors true, also cut the world wherever a building hides the character
// from the camera under the open sky. ARPG_SetCutaway() switches it all off.
int __cdecl LuaSetCutaway(game::lua_State* L) {
    g_settings.enabled = game::lua_type(L, 1) == game::LUA_TNUMBER;
    if (g_settings.enabled) {
        const float height = static_cast<float>(game::lua_tonumber(L, 1));
        g_settings.height = Max(kMinHeight, Min(kMaxHeight, height));
        g_settings.outdoors = game::lua_gettop(L) >= 2 && game::lua_toboolean(L, 2) != 0;
    }
    return 0;
}

// asked, cutting, height, frames, framesCut, framesSkipped, rooms, indoors,
// blocked, refused, overhead, between, inside, framesInside = ARPG_CutawayInfo()
//   asked          1 while a cutaway is asked for
//   cutting        1 while the world is being cut right now
//   height         yards over the character's feet of the cut, 0 when not cutting
//   frames         scene updates since the UI loaded
//   framesCut      how many of them were drawn cut
//   framesSkipped  how many were due a cut but the device's matrices were not as expected
//   rooms          rooms drawn in the last frame of the inside view (or queued in the last frame cut without it)
//   indoors        1 if the character is in a building: found there by the scene, or so says the client
//   blocked        1 if, under the open sky, a building stands between camera and character
//   refused        1 if the cut has been given up: the device does not keep its projection
//   overhead       yards over the feet of the building overhead, 0 if none was found (looked for indoors only)
//   between        yards over the feet of the lowest part of a building between camera and character, 0 if
//                  none (not looked for in the inside view)
//   inside         1 if the scene found the character's room and the last frame was drawn from it
//   framesInside   scene draws made that way since the UI loaded
int __cdecl LuaCutawayInfo(game::lua_State* L) {
    const auto flag = [](bool value) { return value ? 1.0 : 0.0; };
    game::lua_pushnumber(L, flag(g_settings.enabled));
    game::lua_pushnumber(L, flag(g_state.cutting));
    game::lua_pushnumber(L, g_state.cutting ? static_cast<double>(g_state.planeZ - g_state.feet.z) : 0.0);
    game::lua_pushnumber(L, static_cast<double>(g_stats.frames));
    game::lua_pushnumber(L, static_cast<double>(g_stats.framesCut));
    game::lua_pushnumber(L, static_cast<double>(g_stats.framesSkipped));
    game::lua_pushnumber(L, static_cast<double>(g_stats.rooms));
    game::lua_pushnumber(L, flag(g_state.indoors));
    game::lua_pushnumber(L, flag(g_state.blocked));
    game::lua_pushnumber(L, flag(g_state.givenUp));
    game::lua_pushnumber(L, static_cast<double>(g_state.overhead));
    game::lua_pushnumber(L, static_cast<double>(g_state.between));
    game::lua_pushnumber(L, flag(g_state.interior));
    game::lua_pushnumber(L, static_cast<double>(g_stats.framesInside));
    return 14;
}

}  // namespace cutaway
