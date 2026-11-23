// The cutaway: seeing the character under a roof.
//
// A camera fixed high above the character is fine in the open and useless
// under a roof: the client draws the roof. What an ARPG shows there is the
// room, looked into from above, and nothing else.
//
// Indoors: the inside view.
//
//   The client can already draw that picture. When it takes the camera to be
//   inside a building it draws the camera's room and what its doorways lead
//   to, and the walls and ceilings of rooms are made to be seen from inside
//   only, so a camera that is really above the roof looks straight through
//   the roof and the near walls at the room. Here it is made to happen
//   wherever the character is in a room:
//
//   1. Which room. The client finds "the building the camera is in" with a
//      line straight down from the camera. The same question is asked from
//      the character's chest. If the answer is a room, the client's routine
//      is run with the character's place for the camera's, so the scene is
//      drawn from that room. "A room" is a group that counts as indoors by
//      the client's own rule (game.h): a city's streets and squares are
//      groups of the city's model too, and standing in one of those is
//      being outdoors.
//   2. The rooms. In place of the client's walk through the doorways, which
//      judges each doorway from where the camera really is and shows a room
//      next door only as far as the doorway frames it, every room that can
//      be reached from the character's through doorways is visited with the
//      whole screen as its frame. A doorway to a street, a courtyard or the
//      outer shell is not gone through, and what is beyond is not drawn.
//   3. Nothing else. No doorway or window onto the outdoor world is opened,
//      so the client draws no street, terrain or sky. The screen is cleared
//      to black, and its depth not to the far end of the view but to just
//      behind the rooms, so that whatever fogs the picture by depth
//      afterwards finds a dark wall there and not a sky to fill with fog.
//      Scenery and creatures are filed under a room by its box, which
//      reaches past its walls: the lamp outside the door and whoever walks
//      by in the street come with it. Each piece is asked where it stands,
//      the way the character was, and left out unless that is a room shown.
//   4. The cut. Beams, galleries and the floor of the storey above are solid
//      and face the camera. Everything above a level plane over the
//      character's feet is clipped away while the world is drawn: the plane
//      is given to Direct3D as a user clip plane, in the clip space the
//      vertex shaders work in, and switched on for each draw of the world
//      made with a vertex shader. Nothing moves on screen and depth stays
//      what it was.
//   5. The cursor. Its ray starts at the camera and would stop at the roof
//      and the near wall, which are not on screen. It is picked up again at
//      the cut and then past every surface it meets until it is inside a
//      room that is drawn.
//
// Outdoors the client draws its own picture from where the camera is, with
// one exception: a camera that does not stop at walls drifts into rooms the
// character is not in, and from in there the client would draw that room
// and leave the character out. The scene is then drawn as from outdoors.
//
// Where the client's lookup finds no room although the client's own indoors
// test says the character is in one: the cut alone.
//
//   The world is drawn as usual and cut at the plane. Every indoor group in
//   view is "visited", which is the client's own way of queueing a room, so
//   that buildings sliced open are not empty. This is the rougher picture:
//   whatever is taller than the cut loses its top, and buildings that are
//   only a shell show it.
//
// Outdoors, when asked, while part of a building hides the character: the
// cut, for what is in the way only.
//
//   The lines of sight from the camera to the character are put to each
//   group that is about to be drawn, with the client's own test of a line
//   against one group's surfaces. The groups they meet are in the way. Only
//   those are cut, with the rooms inside them (which are visited, as above):
//   the plane is on for a draw only while one of them is being drawn. The
//   street, the terrain and the building across the road are drawn whole.
//   Models are not cut one by one: the client draws many of a kind in a
//   single draw. The scenery and the creatures that stand on a part in the
//   way, wholly above the cut, are left out with what is cut away under
//   them; whatever reaches below the cut is drawn whole. If the line of
//   sight is blocked and no group can be named, everything is cut, as
//   indoors.
//
// Where the cut is made:
//   - Under a roof lower than the camera: at a set height over the
//     character's feet. Under a ceiling lower than that, just under the
//     ceiling. In a tall room, just under whatever is in the way.
//   - Under a roof higher than the camera, seen from inside: not at all.
//   - Under the open sky: only if asked, while part of a building stands
//     between the camera and the character, and then just under that; and
//     only that building's parts.
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
constexpr uint32_t kMaxRefusals = 3;     // times Direct3D may refuse the cut's clip plane
constexpr int kMaxUnseen = 6;            // surfaces not on screen a click may pass on its way into a drawn room
constexpr float kProbe = 200.0f;         // yards up and down a point is looked at to tell which room it is in
constexpr float kPast = 0.05f;           // yards past a surface the cursor's ray is picked up again
constexpr uint32_t kBlack = 0xFF000000;
constexpr int kMaxGroups = 1024;         // per building: far more than one has; ends a walk over a broken list
constexpr uint32_t kMaxDoorways = 256;   // per room, likewise
constexpr uint32_t kClipPlane = 5;       // which of Direct3D's six user clip planes the cut uses: the last one
                                         // (the client itself uses the first)
constexpr uint32_t kClipPlaneBit = 1u << kClipPlane;
constexpr float kBehindRooms = 2.0f;     // yards behind the farthest corner of the rooms the background is put
constexpr float kHead = 2.2f;            // yards over the feet: the highest line of sight tried for what is in the way
constexpr float kBeside = 1.0f;          // yards to either side of the character the lines of sight are tried as well
constexpr float kAround = 1.5f;          // yards around a part in the way within which scenery goes with it
constexpr int kMaxInTheWay = 32;         // parts of buildings in the way that are kept track of
constexpr uint32_t kInTheWayMs = 1000;   // a part stays cut this long after it was last in the way: longer than a cut
                                         // takes to end, so that it does not come back whole under a plane still low
constexpr int kMaxQueued = 8192;         // groups on a frame's list; ends a walk over a broken list
constexpr float kStandingLift = 0.5f;    // yards over its centre the look under a piece of scenery or a creature
                                         // begins: what lies flat on a floor, or stands a little sunk into it, is
                                         // still over it
constexpr float kStandingProbe = 300.0f; // yards straight down it goes, for what flies or hangs in the highest hall;
                                         // and straight up, where nothing is found below
constexpr float kOwnReach = 8.0f;        // yards around the character within which a creature is not asked about
constexpr uint32_t kMaxProbes = 256;     // such lookups a frame; pieces beyond that are drawn unasked
constexpr uint32_t kRecheckFrames = 120; // frames after which a piece found outside every room is asked about again

bool g_installed = false;
bool g_canFilter = false;  // the two calls that decide whether scenery and creatures are drawn are redirected too
bool g_canChoose = false;  // ... and the one that tells which building group is being drawn
uint32_t g_frame = 0;      // scene updates since the library was loaded; what is kept per frame is told by it

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

    bool indoors;       // the character is in a building: in a room the scene's lookup found or, failing that,
                        // by the client's indoors test
    bool interior;      // the character is in a room, and this frame is drawn from it: the inside view
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
    bool open;          // the cut, if any, is the open-sky kind: for what is in the way only
    bool sightNow;      // ... and this very frame something is in the line of sight
    bool unnamed;       // ... but no part of a building could be named for it, so the cut is for everything
                        // after all, until one can be or the cut is over
    bool drawnCut;      // the last frame that asked for a cut was drawn with one

    bool givenUp;       // Direct3D does not take the cut's clip plane
    uint32_t refusals;
};
State g_state = {};

// The clip plane on the graphics device, for one frame's drawing of the world.
struct Cut {
    bool on;                  // from just before the world is drawn until it has been
    bool sent;                // Direct3D's clip-plane switch has this library's bit in it
    bool all;                 // every draw of the world is cut; otherwise only the building groups chosen...
    bool piece;               // ... and this says whether the group being drawn right now is one
    game::Matrix projection;  // the hardware projection the plane was worked out for
};
Cut g_cut = {};

// Outdoors: the parts of buildings found between the camera and the
// character. Each is a placed group, known by its building and its number.
struct InTheWay {
    uint8_t* building;  // compared, and followed only in the frame it was found in
    uint32_t group;
    game::Box box;
    uint32_t seen;      // tick it was last in the way
};
struct Way {
    InTheWay parts[kMaxInTheWay];
    int count;
};
Way g_way = {};

// A set of pointers that empties itself every frame: a slot counts only if
// it was written in the current one.
struct Slot {
    const void* pointer;
    uint32_t frame;
};
constexpr uint32_t kLeftOutSlots = 512;  // a power of two
constexpr uint32_t kSlotTries = 8;
Slot g_leftOut[kLeftOutSlots];  // pieces left out this frame, to count each once

// Where a piece of scenery stands, kept from frame to frame: scenery does
// not move. `centre` tells a piece from another that has since been given
// the same address.
struct Standing {
    const void* piece;
    Vec3 centre;
    void* building;  // null: nothing under it that is a room
    uint32_t group;
    uint32_t frame;  // when it was looked up
};
constexpr uint32_t kStandingSlots = 2048;
Standing g_standing[kStandingSlots];

// What the scene's lookup found under the character, for ARPG_CutawayInfo.
struct Where {
    bool found;
    uint32_t group;
    uint32_t flags;
    int32_t area;   // the area table's flags for the group, -1 if it has no record
    bool outdoors;  // by the client's rule
};
Where g_where = {};

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
    uint32_t framesCut;      // frames whose world was drawn with the cut
    uint32_t framesSkipped;  // frames where a cut was due but the device or its matrices were not as expected
    uint32_t framesInside;   // frames drawn as the inside view
    uint32_t rooms;          // rooms drawn in the last frame of the inside view, or queued in the last cut one
    uint32_t doorsOut;       // in that frame of the inside view: places reached through a doorway that are not rooms
    uint32_t draws;          // draws made with the cut in the last frame that had one
    int32_t parts;           // parts of buildings that frame cut for being in the way; -1 if it cut everything
    uint32_t sceneryOut;     // scenery left out of the last frame: for standing outside the rooms shown, or
                             // outdoors on a part that is cut, above the cut
    uint32_t creaturesOut;   // creatures and objects, likewise
    uint32_t sceneryOutNow;  // the same two, counted while a frame is put together
    uint32_t creaturesOutNow;
    uint32_t probes;         // lookups of where a piece stands, this frame
    bool logged[kSkipReasons];
    bool loggedRefusal;
};
Stats g_stats = {};

// The rooms of the last frame drawn as the inside view: for the cursor, and
// for where the background goes.
struct Drawn {
    void* building;  // compared, never followed: it may be gone by the time the cursor asks
    uint32_t groups[kMaxGroups / 32];
    float reach;     // yards from the camera to the farthest corner of them, 0 if none was drawn
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
// and at what height. `inRoom`: the character is in a room, and the frame
// will be drawn from it.
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

    if (g_state.givenUp) {
        // Direct3D does not take the cut. The inside view goes on without.
        g_state.cutting = false;
        g_state.blocked = false;
        g_state.blockPending = false;
        g_state.open = false;
        g_state.sightNow = false;
        return;
    }

    // The cut keeps clear of the character, and a jump takes the cut up with
    // it only for as far as that needs.
    const float room = feet.z + g_settings.height;
    float limit = feet.z + Min(g_settings.height, kHeadroom);

    bool wanted = false;
    float target = 0.0f;
    g_state.open = false;
    g_state.sightNow = false;
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
        // lookup did not find). What is cut here is only what is in the way.
        g_state.open = true;
        g_state.sightNow = sight && g_settings.outdoors;
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
// Which room
// ---------------------------------------------------------------------------

template <typename T>
T Read(const void* base, size_t offset) {
    return *reinterpret_cast<const T*>(static_cast<const uint8_t*>(base) + offset);
}

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

// The building's model, if the building's groups are placed and the model's
// file is in.
uint8_t* ReadyModel(uint8_t* building) {
    if ((At<uint32_t>(building, game::kBuildingFlags) & game::kBuildingFlagGroupsPlaced) == 0) {
        return nullptr;
    }
    uint8_t* model = At<uint8_t*>(building, game::kBuildingModel);
    return model != nullptr && At<uint32_t>(model, game::kModelLoaded) != 0 ? model : nullptr;
}

// A group's own data, if the group can be visited: it exists, is placed and
// its data is in. (The visit routine checks none of that.)
uint8_t* ReadyGroup(uint8_t* building, uint8_t* model, uint32_t index) {
    if (index >= At<uint32_t>(model, game::kModelGroupCount) || PlacedGroupAt(building, index) == nullptr) {
        return nullptr;
    }
    uint8_t* data = (&At<uint8_t*>(model, game::kModelGroups))[index];
    if (data == nullptr || (At<uint32_t>(data, game::kGroupDataLoadFlags) & game::kGroupDataLoaded) == 0) {
        return nullptr;
    }
    return data;
}

// The client's own rule for "outdoors" (game.h), asked of a group: the area
// table's word if it has one, else the group's flag.
bool GroupIsOutdoors(uint8_t* building, uint8_t* model, uint8_t* data, int32_t* area) {
    bool outdoors = (At<uint32_t>(data, game::kGroupDataFlags) & game::kGroupFlagOutdoor) != 0;
    *area = -1;
    const uint8_t* header = At<const uint8_t*>(model, game::kModelHeader);
    const void* record = header == nullptr
                             ? nullptr
                             : game::FindBuildingArea(Read<uint32_t>(header, game::kModelHeaderId),
                                                      At<uint32_t>(building, game::kBuildingNameSet),
                                                      At<uint32_t>(data, game::kGroupDataAreaId));
    if (record != nullptr) {
        const uint32_t flags = Read<uint32_t>(record, game::kAreaFlags);
        *area = static_cast<int32_t>(flags);
        if ((flags & game::kAreaFlagOutdoors) != 0) {
            outdoors = true;
        } else if ((flags & game::kAreaFlagIndoors) != 0) {
            outdoors = false;
        }
    }
    return outdoors;
}

struct Room {
    uint8_t* building;
    uint32_t group;
};

// The client's own question, "which building group is under this point",
// asked the way its camera lookup asks it (0x00795D40): straight down, and
// no further than the terrain. True if the answer is a room. What was found
// goes to `where` either way.
bool RoomBelow(const Vec3& point, float depth, Room* room, Where* where) {
    *where = {};
    if ((*reinterpret_cast<const uint32_t*>(game::kWorldSwitches) & game::kWorldSwitchBuildings) == 0) {
        return false;
    }
    const Vec3 below = {point.x, point.y, point.z - depth};
    float fraction = 1.0f;
    game::IntersectTerrain(point, below, &fraction);
    void* buildings[2] = {nullptr, nullptr};
    uint32_t groups[4] = {game::kNoGroup, game::kNoGroup, game::kNoGroup, game::kNoGroup};
    if (!game::FindGroupOnLine(point, below, fraction, buildings, groups) || buildings[0] == nullptr) {
        return false;
    }
    uint8_t* building = static_cast<uint8_t*>(buildings[0]);
    uint8_t* model = ReadyModel(building);
    uint8_t* data = model != nullptr ? ReadyGroup(building, model, groups[0]) : nullptr;
    if (data == nullptr) {
        return false;
    }
    where->found = true;
    where->group = groups[0];
    where->flags = At<uint32_t>(data, game::kGroupDataFlags);
    where->outdoors = GroupIsOutdoors(building, model, data, &where->area);
    if (where->outdoors || (where->flags & game::kGroupFlagAlwaysOutdoor) != 0) {
        return false;
    }
    *room = {building, groups[0]};
    return true;
}

// The room the character is in. What was found under the character is kept
// for ARPG_CutawayInfo.
bool RoomUnder(const Vec3& point, Room* room) {
    return RoomBelow(point, game::kCameraLookupDepth, room, &g_where);
}

// Outdoors the scene is drawn from wherever the camera is, as the client
// does it. But a camera that does not stop at walls ends up in rooms the
// character is not in, and from in there the client would draw that room and
// what its doorways show: the character, out in the street, would not be in
// the picture. The camera is then taken to be outdoors. In a street or a
// square of a city's model it is left where it is: the client draws those
// properly only from inside them.
void LeaveRoomWithoutCharacter() {
    uint8_t* building = *reinterpret_cast<uint8_t* const*>(game::kSceneCameraBuilding);
    const uint32_t count = *reinterpret_cast<const uint32_t*>(game::kSceneCameraGroupCount);
    const uint32_t* groups = *reinterpret_cast<const uint32_t* const*>(game::kSceneCameraGroups);
    if (building == nullptr || count == 0 || groups == nullptr) {
        return;
    }
    uint8_t* model = ReadyModel(building);
    uint8_t* data = model != nullptr ? ReadyGroup(building, model, groups[0]) : nullptr;
    int32_t area = -1;
    if (data == nullptr || GroupIsOutdoors(building, model, data, &area)) {
        return;
    }
    *reinterpret_cast<void**>(game::kSceneCameraBuilding) = nullptr;
    *reinterpret_cast<void**>(game::kSceneCameraBuildingSecond) = nullptr;
    *reinterpret_cast<uint32_t*>(game::kSceneCameraGroupCount) = 0;
    *reinterpret_cast<uint32_t*>(game::kSceneCameraGroupCountSecond) = 0;
    *reinterpret_cast<char*>(game::kSceneCameraBuildingName) = '\0';
    *reinterpret_cast<char*>(game::kSceneCameraGroupName) = '\0';
}

void StopCut();

using Plain = void (__cdecl*)();
Plain g_originalLocateCamera = nullptr;

void __cdecl LocateCameraHook() {
    ++g_frame;
    ++g_stats.frames;
    g_stats.sceneryOutNow = 0;
    g_stats.creaturesOutNow = 0;
    g_stats.probes = 0;
    g_state.active = false;
    g_state.interior = false;
    StopCut();  // had last frame's drawing not come to its usual end
    void* player = nullptr;
    if (!g_installed || !g_settings.enabled || !game::InWorld() || (player = game::PlayerObject()) == nullptr) {
        g_state.cutting = false;
        g_state.indoors = false;
        g_state.blocked = false;
        g_state.blockPending = false;
        g_state.open = false;
        g_state.sightNow = false;
        g_state.overhead = 0.0f;
        g_state.between = 0.0f;
        g_way.count = 0;
        g_where = {};
        g_originalLocateCamera();
        return;
    }

    // The client's routine asks which building lies under the camera, by a
    // line straight down from kSceneCameraPosition, which it reads once at
    // its start. If the character is in a room, it is asked from the
    // character's chest instead, and the scene is drawn from that room even
    // while the camera itself hangs above the roof or in somebody's attic.
    Vec3 feet;
    game::ObjectPosition(player, &feet);
    const Vec3 chest = {feet.x, feet.y, feet.z + kChest};
    Vec3* camera = reinterpret_cast<Vec3*>(game::kSceneCameraPosition);
    const Vec3 eye = *camera;
    Room room = {};
    bool inRoom = RoomUnder(chest, &room);
    if (inRoom) {
        *camera = chest;
        g_originalLocateCamera();
        *camera = eye;
        inRoom = *reinterpret_cast<uint8_t* const*>(game::kSceneCameraBuilding) == room.building;
    }
    if (!inRoom) {
        g_originalLocateCamera();
        LeaveRoomWithoutCharacter();
    }

    UpdateForFrame(player, feet, eye, inRoom);
    if (!g_state.active || !g_state.open) {
        g_way.count = 0;  // parts in the way belong to a cut under the open sky, and to no other
    }
}

// ---------------------------------------------------------------------------
// Outdoors: what is in the way
// ---------------------------------------------------------------------------

// Whether the cut is for everything this frame draws, or only for the parts
// of buildings in the way. Outdoors it is for those parts; but with the line
// of sight blocked and no part named, the old way is better than none.
bool CutsEverything() {
    return !g_canChoose || g_state.interior || !g_state.open || g_state.unnamed;
}

bool SegmentTouchesBox(const Vec3& from, const Vec3& to, const game::Box& box) {
    const float start[3] = {from.x, from.y, from.z};
    const float step[3] = {to.x - from.x, to.y - from.y, to.z - from.z};
    const float low[3] = {box.low.x, box.low.y, box.low.z};
    const float high[3] = {box.high.x, box.high.y, box.high.z};
    float enter = 0.0f;
    float leave = 1.0f;
    for (int axis = 0; axis < 3; ++axis) {
        if (Abs(step[axis]) < 0.00001f) {
            if (start[axis] < low[axis] || start[axis] > high[axis]) {
                return false;
            }
            continue;
        }
        const float a = (low[axis] - start[axis]) / step[axis];
        const float b = (high[axis] - start[axis]) / step[axis];
        enter = Max(enter, Min(a, b));
        leave = Min(leave, Max(a, b));
        if (enter > leave) {
            return false;
        }
    }
    return true;
}

bool BoxesTouch(const game::Box& a, const game::Box& b) {
    return a.low.x <= b.high.x && b.low.x <= a.high.x && a.low.y <= b.high.y && b.low.y <= a.high.y &&
           a.low.z <= b.high.z && b.low.z <= a.high.z;
}

Vec3 ToModel(const Vec3& point, const Matrix& placement) {
    const float (*m)[4] = placement.m;
    return {point.x * m[0][0] + point.y * m[1][0] + point.z * m[2][0] + m[3][0],
            point.x * m[0][1] + point.y * m[1][1] + point.z * m[2][1] + m[3][1],
            point.x * m[0][2] + point.y * m[1][2] + point.z * m[2][2] + m[3][2]};
}

// Whether the line meets a surface of this one group: the client's own test
// (game.h), asked the way the client asks it, with the surfaces the line of
// sight is judged by.
bool GroupOnLine(uint8_t* building, uint8_t* model, uint32_t index, uint8_t* data, const Vec3& from,
                 const Vec3& to) {
    const Matrix& placement = At<Matrix>(building, game::kBuildingPlacement);
    const Vec3 line[2] = {ToModel(from, placement), ToModel(to, placement)};
    if (!game::LineTouchesGroupBox(model, &line[0], &line[1], index)) {
        return false;
    }
    float fraction = 1.0f;
    uint8_t out = 0;
    return game::LineMeetsGroup(data, line, &fraction, game::kIntersectBuildings, game::kBuildingSurfacesPassed, &out,
                                building) &&
           fraction >= 0.0f && fraction <= 1.0f;
}

// The placed building a placed group belongs to.
uint8_t* BuildingOf(uint8_t* placed) {
    const uintptr_t owner = At<uintptr_t>(placed, game::kPlacedGroupOwner);
    return IsLink(owner) ? At<uint8_t*>(reinterpret_cast<void*>(owner), game::kOwnerBuilding) : nullptr;
}

void RememberInTheWay(uint8_t* building, uint32_t group, const game::Box& box, uint32_t now) {
    int slot = -1;
    for (int i = 0; i < g_way.count; ++i) {
        if (g_way.parts[i].building == building && g_way.parts[i].group == group) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (g_way.count < kMaxInTheWay) {
            slot = g_way.count++;
        } else {
            slot = 0;  // full: the one longest out of the way makes room
            for (int i = 1; i < g_way.count; ++i) {
                if (now - g_way.parts[i].seen > now - g_way.parts[slot].seen) {
                    slot = i;
                }
            }
        }
    }
    g_way.parts[slot] = {building, group, box, now};
}

// Goes through the groups this frame is about to draw for the ones a line of
// sight from the camera to the character meets. Lines to the chest, the head
// and a yard to either side: what hides the ground next to the character
// hides the character a step later.
void FindWhatIsInTheWay() {
    const uint32_t now = g_state.lastTick;
    int kept = 0;
    for (int i = 0; i < g_way.count; ++i) {
        if (now - g_way.parts[i].seen <= kInTheWayMs) {
            g_way.parts[kept++] = g_way.parts[i];
        }
    }
    g_way.count = kept;
    if (!g_state.sightNow) {
        return;
    }
    if (*reinterpret_cast<const int32_t*>(game::kQueuedGroupsLinkOffset) + static_cast<int32_t>(sizeof(void*)) !=
        static_cast<int32_t>(game::kPlacedGroupQueued)) {
        return;  // the list is not laid out as read
    }

    const Vec3& feet = g_state.feet;
    const Vec3& eye = g_state.eye;
    float sideX = feet.y - eye.y;
    float sideY = eye.x - feet.x;
    const float length = Sqrt(sideX * sideX + sideY * sideY);
    if (length > 0.001f) {
        sideX = sideX / length * kBeside;
        sideY = sideY / length * kBeside;
    } else {
        sideX = kBeside;
        sideY = 0.0f;
    }
    const Vec3 targets[] = {
        {feet.x, feet.y, feet.z + kChest},
        {feet.x, feet.y, feet.z + kHead},
        {feet.x + sideX, feet.y + sideY, feet.z + kChest},
        {feet.x - sideX, feet.y - sideY, feet.z + kChest},
    };

    uintptr_t next = *reinterpret_cast<const uintptr_t*>(game::kQueuedGroupsHead);
    for (int guard = 0; IsLink(next) && guard < kMaxQueued; ++guard) {
        uint8_t* placed = reinterpret_cast<uint8_t*>(next);
        next = At<uintptr_t>(placed, game::kPlacedGroupQueued);
        const game::Box& box = At<game::Box>(placed, game::kPlacedGroupBounds);
        uint8_t* building = BuildingOf(placed);
        uint8_t* model = building != nullptr ? ReadyModel(building) : nullptr;
        if (model == nullptr) {
            continue;
        }
        const uint32_t index = At<uint32_t>(placed, game::kPlacedGroupIndex);
        uint8_t* data = ReadyGroup(building, model, index);
        if (data == nullptr || PlacedGroupAt(building, index) != placed) {
            continue;
        }
        for (const Vec3& target : targets) {
            // Both ways, as the line of sight itself is tried.
            if (SegmentTouchesBox(target, eye, box) && (GroupOnLine(building, model, index, data, target, eye) ||
                                                         GroupOnLine(building, model, index, data, eye, target))) {
                RememberInTheWay(building, index, box, now);
                break;
            }
        }
    }
}

bool IsInTheWay(const uint8_t* building, uint32_t group) {
    for (int i = 0; i < g_way.count; ++i) {
        if (g_way.parts[i].building == building && g_way.parts[i].group == group) {
            return true;
        }
    }
    return false;
}

bool HasPartInTheWay(const uint8_t* building) {
    for (int i = 0; i < g_way.count; ++i) {
        if (g_way.parts[i].building == building) {
            return true;
        }
    }
    return false;
}

// A room goes with a part in the way if it is of the same building and their
// boxes touch: the rooms inside a house whose shell is being cut.
bool TouchesPartInTheWay(const uint8_t* building, const game::Box& box) {
    for (int i = 0; i < g_way.count; ++i) {
        if (g_way.parts[i].building == building && BoxesTouch(g_way.parts[i].box, box)) {
            return true;
        }
    }
    return false;
}

// Scenery and creatures go with a part in the way if they stand in its box,
// right by it or anywhere over it: the sign over the door, the lantern on
// the wall, the chimney on the roof and the flag on the chimney.
bool StandsByPartInTheWay(const Vec3& point) {
    for (int i = 0; i < g_way.count; ++i) {
        const game::Box& box = g_way.parts[i].box;
        if (point.x >= box.low.x - kAround && point.x <= box.high.x + kAround && point.y >= box.low.y - kAround &&
            point.y <= box.high.y + kAround && point.z >= box.low.z - kAround) {
            return true;
        }
    }
    return false;
}

// Adds to a set that empties itself every frame. True if the pointer was not
// in it yet. A set that is full where the pointer belongs does not take it.
bool AddThisFrame(Slot* slots, uint32_t count, const void* pointer) {
    const uint32_t frame = g_frame;
    const uint32_t first = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pointer) >> 4);
    for (uint32_t i = 0; i < kSlotTries; ++i) {
        Slot& slot = slots[(first + i) & (count - 1)];
        if (slot.frame != frame) {
            slot = {pointer, frame};
            return true;
        }
        if (slot.pointer == pointer) {
            return false;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// The rooms
// ---------------------------------------------------------------------------

constexpr int kMaxBuildings = 4096;  // far more than a scene holds; ends a walk over a broken list

// The cut without the inside view: every indoor group of a building that is
// under the cut and in view; where only what is in the way is cut, only the
// rooms that go with that.
void ShowRoomsOf(uint8_t* building, bool everything) {
    uint8_t* model = ReadyModel(building);
    if (model == nullptr) {
        return;
    }
    const game::Box& bounds = At<game::Box>(building, game::kBuildingBounds);
    if (bounds.low.z >= g_state.planeZ || game::BoxOutsideView(&bounds)) {
        return;
    }
    const uint32_t groupCount = At<uint32_t>(model, game::kModelGroupCount);
    const uint8_t* groupInfo = At<const uint8_t*>(model, game::kModelGroupInfo);
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
        const uint32_t flags = Read<uint32_t>(groupInfo, index * game::kModelGroupInfoSize);
        if ((flags & (game::kGroupFlagOutdoor | game::kGroupFlagAlwaysOutdoor)) != 0) {
            continue;  // the outdoor pass has these
        }
        if (ReadyGroup(building, model, index) == nullptr) {
            continue;  // still loading
        }
        const game::Box& box = At<game::Box>(placed, game::kPlacedGroupBounds);
        if (box.low.z >= g_state.planeZ || game::BoxOutsideView(&box)) {
            continue;  // wholly above the cut, or off screen
        }
        if (!everything && !TouchesPartInTheWay(building, box)) {
            continue;  // inside a building that stays whole
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
    const bool everything = CutsEverything();
    if (!everything && g_way.count == 0) {
        return;
    }
    const int32_t linkOffset = *reinterpret_cast<const int32_t*>(game::kPlacedBuildingsLinkOffset);
    uintptr_t building = *reinterpret_cast<const uintptr_t*>(game::kPlacedBuildingsHead);
    for (int guard = 0; IsLink(building) && guard < kMaxBuildings; ++guard) {
        uint8_t* current = reinterpret_cast<uint8_t*>(building);
        building = At<uintptr_t>(current, static_cast<size_t>(linkOffset) + sizeof(void*));
        if (everything || HasPartInTheWay(current)) {
            ShowRoomsOf(current, everything);
        }
    }
    *reinterpret_cast<uint32_t*>(game::kSceneInteriorPath) = interiorPath;
}

// How far the farthest corner of a box is from a point.
float FarthestCorner(const game::Box& box, const Vec3& from) {
    const float x = Max(Abs(box.low.x - from.x), Abs(box.high.x - from.x));
    const float y = Max(Abs(box.low.y - from.y), Abs(box.high.y - from.y));
    const float z = Max(Abs(box.low.z - from.z), Abs(box.high.z - from.z));
    return Sqrt(x * x + y * y + z * z);
}

// The inside view: every room that can be reached from the character's
// through doorways, each visited under the view the scene is in (the whole
// screen), which queues it and what is in it for drawing. A doorway is gone
// through whatever the camera can see of it. What it leads to is looked at
// first: a group that counts as outdoors (a street, a courtyard, the outer
// shell) is neither visited nor gone through, so the house across the street
// is not drawn either. `list` is the client's list of the groups the camera
// is in: capacity, count, indexes.
void ShowFromInside(uint8_t* building, const void* list) {
    g_stats.rooms = 0;
    g_stats.doorsOut = 0;
    g_drawn = {};
    uint8_t* model = ReadyModel(building);
    if (model == nullptr || list == nullptr) {
        return;
    }
    const game::Doorway* doorways = At<const game::Doorway*>(model, game::kModelDoorways);

    uint32_t reached[kMaxGroups / 32] = {};
    uint16_t queue[kMaxGroups];
    int waiting = 0;
    const auto reach = [&](uint32_t index) {  // (a doorway with nothing beyond it has game::kNoGroup there)
        if (index < static_cast<uint32_t>(kMaxGroups) && (reached[index / 32] >> (index % 32) & 1u) == 0) {
            reached[index / 32] |= 1u << (index % 32);
            queue[waiting++] = static_cast<uint16_t>(index);
        }
    };
    const uint32_t startCount = Read<uint32_t>(list, sizeof(uint32_t));
    const uint32_t* start = Read<const uint32_t*>(list, 2 * sizeof(uint32_t));
    for (uint32_t i = 0; start != nullptr && i < startCount && i < 4; ++i) {
        reach(start[i]);
    }

    const uint32_t interiorPath = *reinterpret_cast<const uint32_t*>(game::kSceneInteriorPath);
    for (int next = 0; next < waiting; ++next) {
        const uint32_t index = queue[next];
        uint8_t* data = ReadyGroup(building, model, index);
        if (data == nullptr) {
            continue;  // still loading: neither its doorways nor what it is are known yet
        }
        const uint32_t flags = At<uint32_t>(data, game::kGroupDataFlags);
        int32_t area = -1;
        if ((flags & game::kGroupFlagAlwaysOutdoor) != 0 || GroupIsOutdoors(building, model, data, &area)) {
            ++g_stats.doorsOut;
            continue;
        }
        const uint32_t first = At<uint32_t>(data, game::kGroupDataFirstDoorway);
        const uint32_t count = At<uint32_t>(data, game::kGroupDataDoorwayCount);
        for (uint32_t i = 0; doorways != nullptr && i < count && i < kMaxDoorways; ++i) {
            reach(doorways[first + i].group);
        }
        const game::Box& box = At<game::Box>(PlacedGroupAt(building, index), game::kPlacedGroupBounds);
        if ((g_state.cutting && box.low.z >= g_state.planeZ) || game::BoxOutsideView(&box)) {
            continue;  // wholly above the cut, or off screen
        }
        *reinterpret_cast<uint32_t*>(game::kSceneInteriorPath) = (flags & game::kGroupFlagOutdoorLit) != 0 ? 0 : 1;
        game::VisitBuildingGroup(index, building);
        g_drawn.groups[index / 32] |= 1u << (index % 32);
        g_drawn.reach = Max(g_drawn.reach, FarthestCorner(box, g_state.eye));
        ++g_stats.rooms;
    }
    *reinterpret_cast<uint32_t*>(game::kSceneInteriorPath) = interiorPath;
    g_drawn.building = building;
}

using WalkRooms = void (__thiscall*)(uint8_t* building, void* groups);
WalkRooms g_originalWalkRooms = nullptr;

// In place of the client's walk from the camera's room through the doorways.
// That walk is also what opens the window onto the outdoor world, which so
// stays shut. The window onto the sky is shut here: the client's lookup
// opens it by itself for a room it shows the sky from.
void __fastcall WalkRoomsHook(uint8_t* building, void* /*edx*/, void* groups) {
    if (!g_state.interior || building != *reinterpret_cast<uint8_t* const*>(game::kSceneCameraBuilding)) {
        g_originalWalkRooms(building, groups);
        return;
    }
    ShowFromInside(building, groups);
    *reinterpret_cast<game::OutdoorWindow*>(game::kSceneSkyWindow) = game::kNoOutdoorWindow;
    ++g_stats.framesInside;
}

Plain g_originalFinishVisits = nullptr;

void __cdecl FinishVisitsHook() {
    if (g_state.active && !g_state.interior) {
        if (g_state.open && g_canChoose) {
            FindWhatIsInTheWay();
            if (g_way.count != 0) {
                g_state.unnamed = false;
            } else if (g_state.sightNow) {
                g_state.unnamed = true;
            }
        }
        ShowRooms();
    }
    g_originalFinishVisits();
    g_stats.sceneryOut = g_stats.sceneryOutNow;
    g_stats.creaturesOut = g_stats.creaturesOutNow;
}

// ---------------------------------------------------------------------------
// Scenery and creatures
// ---------------------------------------------------------------------------

bool IsDrawnRoom(void* building, uint32_t group);
bool RoomOnLine(const Vec3& from, const Vec3& to, void** building, uint32_t* group);

// The room a piece of scenery or a creature stands in. What is under it is
// asked the way the character's own room was: the terrain, then the nearest
// building surface above that. Where there is none, the floor is the terrain
// itself or the piece is set into the floor, and the nearest surface over it
// says where it is: a ceiling, or the floor it is set into. A street or a
// roof under it settles it: that is not a room, whatever is overhead.
bool StandsIn(const Vec3& centre, Room* room) {
    const Vec3 from = {centre.x, centre.y, centre.z + kStandingLift};
    Where where = {};
    if (RoomBelow(from, kStandingProbe, room, &where)) {
        return true;
    }
    if (where.found) {
        return false;
    }
    const Vec3 above = {from.x, from.y, from.z + kStandingProbe};
    void* building = nullptr;
    uint32_t group = 0;
    if (!RoomOnLine(from, above, &building, &group)) {
        return false;
    }
    *room = {static_cast<uint8_t*>(building), group};
    return true;
}

// Whether a point is in one of the rooms the inside view is showing.
bool InShownRoom(const Vec3& point) {
    Room room = {};
    return StandsIn(point, &room) && IsDrawnRoom(room.building, room.group);
}

// The same for a piece of scenery, which stays where it is: the room it
// stands in is looked up once and kept. One found in no room is asked about
// again after a while, in case the floor under it had not been loaded yet.
bool SceneryInShownRoom(const uint8_t* piece, const Vec3& centre) {
    Standing& kept = g_standing[(reinterpret_cast<uintptr_t>(piece) >> 4) & (kStandingSlots - 1)];
    const bool same = kept.piece == piece && kept.centre.x == centre.x && kept.centre.y == centre.y &&
                      kept.centre.z == centre.z;
    if (!same || (kept.building == nullptr && g_frame - kept.frame >= kRecheckFrames)) {
        if (g_stats.probes >= kMaxProbes) {
            return true;
        }
        ++g_stats.probes;
        Room room = {};
        const bool found = StandsIn(centre, &room);
        kept = {piece, centre, found ? room.building : nullptr, found ? room.group : 0, g_frame};
    }
    return kept.building != nullptr && IsDrawnRoom(kept.building, kept.group);
}

// Whatever is within reach of the character is always drawn: the character
// itself, its mount and its pet; and whoever stands close enough to hit it,
// which nobody wants to be invisible for standing on a doorstep that counts
// as outdoors, or on a ledge above the cut.
bool WithinReach(const Vec3& centre) {
    const Vec3& feet = g_state.feet;
    const float aside = (centre.x - feet.x) * (centre.x - feet.x) + (centre.y - feet.y) * (centre.y - feet.y);
    return aside <= kOwnReach * kOwnReach && Abs(centre.z - feet.z) <= kOwnReach;
}

// Creatures move, and there are few of them: each is asked about every
// frame.
bool CreatureInShownRoom(const Vec3& centre) {
    if (g_stats.probes >= kMaxProbes) {
        return true;
    }
    ++g_stats.probes;
    return InShownRoom(centre);
}

// Outdoors, with only the parts in the way cut: whether a piece goes with
// what is cut away. A model cannot be cut by itself, because the client
// draws many of a kind in a single draw (0x00820AE0) and the plane is on or
// off for all of it. So what stands on or by a part in the way and wholly
// above the cut is left out: the sign over the door, the chimney, the
// furniture of the storey that is gone, whoever stands up there. What
// reaches below the cut is drawn whole, as is everything that stands
// somewhere else. And nothing is left out unless the cut is really being
// made: whether this frame's will be is not known yet when the scene asks
// (the device may not be one this library draws on, or its matrices not what
// they are taken to be), so the frame before stands in for it.
bool GoesWithTheCut(const Vec3& centre, float radius) {
    return g_state.drawnCut && g_way.count != 0 && centre.z - radius > g_state.planeZ &&
           StandsByPartInTheWay(centre);
}

// A piece of scenery or a creature shows in the view of a group being gone
// through for what is in it. Whether it is drawn: in the inside view, only
// if it stands in a room that is shown; outdoors, unless it goes with what
// is cut away.
bool KeepPiece(const Vec3* centre, float radius, bool scenery) {
    if (!g_canFilter) {
        return true;
    }
    const uint8_t* piece = reinterpret_cast<const uint8_t*>(centre) - game::kSceneCentre;
    bool keep = true;
    if (!scenery && WithinReach(*centre)) {
        keep = true;
    } else if (g_state.interior) {
        keep = scenery ? SceneryInShownRoom(piece, *centre) : CreatureInShownRoom(*centre);
    } else {
        keep = !GoesWithTheCut(*centre, radius);
    }
    if (!keep && AddThisFrame(g_leftOut, kLeftOutSlots, piece)) {
        ++(scenery ? g_stats.sceneryOutNow : g_stats.creaturesOutNow);
    }
    return keep;
}

using InView = int (__thiscall*)(void* view, const Vec3* centre, float radius);
InView g_originalSceneryInView = nullptr;
InView g_originalCreatureInView = nullptr;

int __fastcall SceneryInViewHook(void* view, void* /*edx*/, const Vec3* centre, float radius) {
    const int seen = g_originalSceneryInView(view, centre, radius);
    return seen != 0 && !KeepPiece(centre, radius, true) ? 0 : seen;
}

int __fastcall CreatureInViewHook(void* view, void* /*edx*/, const Vec3* centre, float radius) {
    const int seen = g_originalCreatureInView(view, centre, radius);
    return seen != 0 && !KeepPiece(centre, radius, false) ? 0 : seen;
}

// ---------------------------------------------------------------------------
// The graphics device
// ---------------------------------------------------------------------------

bool IsZero(float value) {
    return Abs(value) < 1e-6f;
}

// The shape documented in game.h.
bool IsWorldProjection(const Matrix& p) {
    return p.m[0][0] > 0.0f && IsZero(p.m[0][1]) && IsZero(p.m[0][2]) && IsZero(p.m[0][3]) &&
           IsZero(p.m[1][0]) && p.m[1][1] > 0.0f && IsZero(p.m[1][2]) && IsZero(p.m[1][3]) &&
           IsZero(p.m[2][0]) && IsZero(p.m[2][1]) && p.m[2][2] > 0.0f && Abs(p.m[2][3] - 1.0f) < 1e-4f &&
           IsZero(p.m[3][0]) && IsZero(p.m[3][1]) && p.m[3][2] < 0.0f && IsZero(p.m[3][3]);
}

bool SameMatrix(const Matrix& a, const Matrix& b) {
    const uint32_t* x = reinterpret_cast<const uint32_t*>(&a);
    const uint32_t* y = reinterpret_cast<const uint32_t*>(&b);
    for (size_t i = 0; i < sizeof(Matrix) / sizeof(uint32_t); ++i) {
        if (x[i] != y[i]) {
            return false;
        }
    }
    return true;
}

// The client's graphics device if it is one of its Direct3D 9 devices and has
// its Direct3D object, else null: with its OpenGL device nothing here applies.
uint8_t* Direct3DDevice(void** direct3d) {
    uint8_t* device = game::GxDevice();
    if (device == nullptr || !game::IsDirect3DDevice(device)) {
        return nullptr;
    }
    *direct3d = At<void*>(device, game::kGxDeviceDirect3D);
    return *direct3d != nullptr ? device : nullptr;
}

// What the client itself has switched on among Direct3D's clip planes.
uint32_t ClientsClipPlanes(uint8_t* device) {
    const uint32_t sent = At<uint32_t>(device, game::kGxDeviceSentClipPlanes);
    return sent == game::kGxNothingSent ? 0 : sent;
}

// ---------------------------------------------------------------------------
// Nothing else: a black screen behind the rooms
// ---------------------------------------------------------------------------

using SceneClear = void (__cdecl*)(uint32_t what, uint32_t colour);
SceneClear g_originalSceneClear = nullptr;

// The client clears the screen to the fog's colour and its depth to the far
// end of the view. In the inside view the colour is black. And the depth is
// that of a wall standing just behind the farthest room: nothing that is
// drawn lies beyond it, and whatever goes over the finished picture by depth
// (a fog shader, say) finds a wall at room distance to fog, and not open
// sky.
void __cdecl SceneClearHook(uint32_t what, uint32_t colour) {
    if (!g_state.interior) {
        g_originalSceneClear(what, colour);
        return;
    }
    g_originalSceneClear(what, kBlack);
    void* direct3d = nullptr;
    uint8_t* device = Direct3DDevice(&direct3d);
    if (device == nullptr || !(g_drawn.reach > 0.0f)) {
        return;
    }
    const Matrix& hardware = At<Matrix>(device, game::kGxDeviceHardwareProjection);
    if (!IsWorldProjection(hardware)) {
        return;
    }
    // Depth at view distance d is m[2][2] + m[3][2] / d: 0 at the near
    // distance, 1 at the far one.
    const float depth = hardware.m[2][2] + hardware.m[3][2] / (g_drawn.reach + kBehindRooms);
    if (depth > 0.0f && depth < 1.0f) {
        game::Direct3DClearDepth(direct3d, depth);
    }
}

// ---------------------------------------------------------------------------
// The cut
// ---------------------------------------------------------------------------

void Skip(SkipReason reason, int a, int b, int c) {
    ++g_stats.framesSkipped;
    g_state.drawnCut = false;
    if (!g_stats.logged[reason]) {
        g_stats.logged[reason] = true;
        static const char* const kWhy[kSkipReasons] = {
            "the client is not drawing with Direct3D 9",
            "the projection matrix is not the one the client builds for the world",
            "the view matrix is not a plain rotation",
            "the character is not where the matrices say the camera is looking",
            "the cut plane does not suit this view",
        };
        logfile::Write("Cutaway: no cut, because %s (%d, %d, %d). Logged once; ARPG_CutawayInfo counts the rest.",
                       kWhy[reason], a, b, c);
    }
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

// Hands Direct3D the cut as a clip plane. Runs once the client has decided
// what is in view, just before it draws; the plane is then switched on draw
// by draw (below) until the world has been drawn.
void StartCut() {
    void* direct3d = nullptr;
    uint8_t* device = Direct3DDevice(&direct3d);
    if (device == nullptr) {
        Skip(kSkipNoDevice, 0, 0, 0);
        return;
    }
    const Matrix projection = At<Matrix>(device, game::kGxDeviceProjection);
    const Matrix hardware = At<Matrix>(device, game::kGxDeviceHardwareProjection);
    const uint32_t level = At<uint32_t>(device, game::kGxDeviceViewLevel);
    if (!IsWorldProjection(projection) || !IsWorldProjection(hardware)) {
        Skip(kSkipProjection, Thousandths(hardware.m[2][2]), Thousandths(hardware.m[2][3]),
             Thousandths(hardware.m[3][3]));
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

    // A point p of the world is (p - eye) * view in view space. The
    // character is on screen in front of the camera; if these matrices put
    // it anywhere else, they do not mean what this code takes them to mean,
    // and it keeps its hands off.
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
    if (!(plane[3] < 0.0f)) {
        Skip(kSkipPlane, Thousandths(plane[3]), 0, 0);
        return;
    }

    // The same plane in clip space. A view-space point (x, y, z, 1) leaves a
    // vertex shader as (x * m00, y * m11, z * m22 + m32, z), with m the
    // hardware projection; dotted with the four numbers below that is the
    // view-space plane's value at the point again.
    const float clip[4] = {plane[0] / hardware.m[0][0], plane[1] / hardware.m[1][1], plane[3] / hardware.m[3][2],
                           plane[2] - hardware.m[2][2] * plane[3] / hardware.m[3][2]};
    if (game::Direct3DSetClipPlane(direct3d, kClipPlane, clip) < 0) {
        if (!g_stats.loggedRefusal) {
            g_stats.loggedRefusal = true;
            logfile::Write("Cutaway: Direct3D does not take user clip plane %d. No cut.", static_cast<int>(kClipPlane));
        }
        ++g_stats.framesSkipped;
        g_state.drawnCut = false;
        if (++g_state.refusals >= kMaxRefusals) {
            g_state.givenUp = true;
            g_state.cutting = false;
        }
        return;
    }
    g_cut.on = true;
    g_state.drawnCut = true;
    g_cut.all = CutsEverything();
    g_cut.piece = false;
    g_cut.projection = hardware;
    g_stats.draws = 0;
    g_stats.parts = g_cut.all ? -1 : g_way.count;
    ++g_stats.framesCut;
}

// The world has been drawn (or a new frame has begun without that): the
// plane is switched off again, and Direct3D has the client's own choice of
// clip planes back.
void StopCut() {
    g_cut.on = false;
    if (!g_cut.sent) {
        return;
    }
    g_cut.sent = false;
    void* direct3d = nullptr;
    uint8_t* device = Direct3DDevice(&direct3d);
    if (device != nullptr) {
        game::Direct3DSetRenderState(direct3d, game::kDirect3DClipPlaneEnable, ClientsClipPlanes(device));
    }
}

// Before every draw, once the client has sent Direct3D its own states for it.
// While the world is being drawn cut, the plane is switched on for a draw
// that is made with a vertex shader and with the world's projection: those
// are the draws the plane is right for. For any other it is switched off.
// Where only the parts in the way are cut, it is on only while one of those
// is being drawn.
// The client's own clip planes are left as the client has them. The switch
// is set afresh each time rather than remembered: the client sends its own
// value whenever that changes, and another library drawing in between may
// have set its own.
void AfterDrawSync(uint8_t* device) {
    if (!g_cut.on && !g_cut.sent) {
        return;
    }
    void* direct3d = At<void*>(device, game::kGxDeviceDirect3D);
    if (direct3d == nullptr) {
        return;
    }
    bool cut = g_cut.on && (g_cut.all || g_cut.piece);
    if (cut) {
        uint8_t* states = At<uint8_t*>(device, game::kGxDeviceStates);
        cut = states != nullptr && At<void*>(states, game::kGxStateVertexShader * game::kGxStateSize) != nullptr &&
              SameMatrix(At<Matrix>(device, game::kGxDeviceHardwareProjection), g_cut.projection);
    }
    if (cut) {
        game::Direct3DSetRenderState(direct3d, game::kDirect3DClipPlaneEnable, ClientsClipPlanes(device) | kClipPlaneBit);
        g_cut.sent = true;
        ++g_stats.draws;
    } else if (g_cut.sent) {
        game::Direct3DSetRenderState(direct3d, game::kDirect3DClipPlaneEnable, ClientsClipPlanes(device));
        g_cut.sent = false;
    }
}

using DrawSync = void (__thiscall*)(uint8_t* device);
DrawSync g_originalDrawSync = nullptr;
DrawSync g_originalDrawSyncEx = nullptr;

void __fastcall DrawSyncHook(uint8_t* device, void* /*edx*/) {
    g_originalDrawSync(device);
    AfterDrawSync(device);
}

void __fastcall DrawSyncExHook(uint8_t* device, void* /*edx*/) {
    g_originalDrawSyncEx(device);
    AfterDrawSync(device);
}

// One group of a building is drawn. Where only the parts in the way are cut:
// whether this is one, or a room that goes with one.
using DrawGroup = void (__thiscall*)(uint8_t* model, uint32_t group, uint8_t* placement, void* views);
DrawGroup g_originalDrawGroup = nullptr;

// A room: by the client's own rule for indoors, as for the character's. A
// street or a courtyard of the same model lies next to the house as well,
// and is not to lose whatever of it is higher than the cut.
bool GroupIsCut(uint8_t* model, uint32_t group, uint8_t* building) {
    if (IsInTheWay(building, group)) {
        return true;
    }
    if (!HasPartInTheWay(building) || ReadyModel(building) != model) {
        return false;
    }
    uint8_t* data = ReadyGroup(building, model, group);
    if (data == nullptr || (At<uint32_t>(data, game::kGroupDataFlags) & game::kGroupFlagAlwaysOutdoor) != 0) {
        return false;
    }
    int32_t area = -1;
    if (GroupIsOutdoors(building, model, data, &area)) {
        return false;
    }
    const uint8_t* placed = static_cast<const uint8_t*>(PlacedGroupAt(building, group));
    return TouchesPartInTheWay(building, Read<game::Box>(placed, game::kPlacedGroupBounds));
}

void __fastcall DrawGroupHook(uint8_t* model, void* /*edx*/, uint32_t group, uint8_t* placement, void* views) {
    g_cut.piece = g_cut.on && !g_cut.all && model != nullptr && placement != nullptr &&
                  GroupIsCut(model, group, placement - game::kBuildingPlacement);
    g_originalDrawGroup(model, group, placement, views);
    g_cut.piece = false;
}

Plain g_originalBeforeDrawing = nullptr;

void __cdecl BeforeDrawingHook() {
    g_originalBeforeDrawing();
    if (g_state.active) {
        g_state.active = false;  // one scene update prepares one draw
        StartCut();
    }
}

using WorldDrawn = void (__cdecl*)(uint32_t list, const Vec3* from);
WorldDrawn g_originalWorldDrawn = nullptr;

// The last of the world's solid things have been drawn. What follows (the
// sun's glare, floating text, names over heads, the interface) is not cut.
void __cdecl WorldDrawnHook(uint32_t list, const Vec3* from) {
    g_originalWorldDrawn(list, from);
    StopCut();
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
    uint32_t groups[4] = {game::kNoGroup, game::kNoGroup, game::kNoGroup, game::kNoGroup};
    if (!game::FindGroupOnLine(from, to, 1.0f, buildings, groups) || buildings[0] == nullptr) {
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
    const hook::Site drawing = {"Cutaway: clip plane", game::kBeforeDrawingCall, game::kBeforeDrawingCallBytes,
                                sizeof(game::kBeforeDrawingCallBytes), 0};
    const hook::Site clear = {"Cutaway: background", game::kSceneClearCall, game::kSceneClearCallBytes,
                              sizeof(game::kSceneClearCallBytes), 0};
    const hook::Site sync = {"Cutaway: draws", game::kDrawSyncCall, game::kDrawSyncCallBytes,
                             sizeof(game::kDrawSyncCallBytes), 0};
    const hook::Site syncEx = {"Cutaway: draws (d3d9ex)", game::kDrawSyncExCall, game::kDrawSyncExCallBytes,
                               sizeof(game::kDrawSyncExCallBytes), 0};
    const hook::Site drawn = {"Cutaway: world drawn", game::kWorldDrawnCall, game::kWorldDrawnCallBytes,
                              sizeof(game::kWorldDrawnCallBytes), 0};
    const hook::Site ray = {"Cutaway: cursor ray", game::kCursorRayCall, game::kCursorRayCallBytes,
                            sizeof(game::kCursorRayCallBytes), 0};
    const hook::Site scenery = {"Cutaway: scenery in view", game::kSceneryInViewCall, game::kSceneryInViewCallBytes,
                                sizeof(game::kSceneryInViewCallBytes), 0};
    const hook::Site creatures = {"Cutaway: creatures in view", game::kCreatureInViewCall,
                                  game::kCreatureInViewCallBytes, sizeof(game::kCreatureInViewCallBytes), 0};
    const hook::Site group = {"Cutaway: building group drawn", game::kDrawGroupCall, game::kDrawGroupCallBytes,
                              sizeof(game::kDrawGroupCallBytes), 0};

    // Each hook passes straight through while g_installed is false, so a
    // partial set does no harm; the cutaway only runs with all nine.
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
    all &= hook::IsActive(hook::RedirectCall(sync, reinterpret_cast<void*>(DrawSyncHook),
                                             reinterpret_cast<void**>(&g_originalDrawSync)));
    all &= hook::IsActive(hook::RedirectCall(syncEx, reinterpret_cast<void*>(DrawSyncExHook),
                                             reinterpret_cast<void**>(&g_originalDrawSyncEx)));
    all &= hook::IsActive(hook::RedirectCall(drawn, reinterpret_cast<void*>(WorldDrawnHook),
                                             reinterpret_cast<void**>(&g_originalWorldDrawn)));
    all &= hook::IsActive(hook::RedirectCall(ray, reinterpret_cast<void*>(CursorRayHook),
                                             reinterpret_cast<void**>(&g_originalCursorRay)));
    g_installed = all;

    // Three more, for telling pieces apart. Without the two that see
    // scenery and creatures, those outside the rooms shown are drawn with
    // them; without those two or the third, which sees each building group
    // drawn, the cut under the open sky is for everything. Either way the
    // rest works as it did.
    bool filter = true;
    filter &= hook::IsActive(hook::RedirectCall(scenery, reinterpret_cast<void*>(SceneryInViewHook),
                                                reinterpret_cast<void**>(&g_originalSceneryInView)));
    filter &= hook::IsActive(hook::RedirectCall(creatures, reinterpret_cast<void*>(CreatureInViewHook),
                                                reinterpret_cast<void**>(&g_originalCreatureInView)));
    const bool choose = hook::IsActive(hook::RedirectCall(group, reinterpret_cast<void*>(DrawGroupHook),
                                                          reinterpret_cast<void**>(&g_originalDrawGroup)));
    g_canFilter = all && filter;
    g_canChoose = all && filter && choose;
    if (all && !g_canChoose) {
        logfile::Write("Cutaway: %s", g_canFilter ? "cannot tell building groups apart as they are drawn; under the "
                                                    "open sky the cut is for everything"
                                                  : "cannot tell where scenery and creatures stand; they are drawn "
                                                    "wherever the game files them, and under the open sky the cut "
                                                    "is for everything");
    }
    return all;
}

void Reset() {
    StopCut();
    g_settings.enabled = false;
    g_state = {};
    g_stats = {};
    g_drawn = {};
    g_where = {};
    g_way = {};
    g_cut.piece = false;
}

// ARPG_SetCutaway(height [, outdoors]): while the character is in a room,
// show the building from inside and nothing else, with whatever is more than
// `height` yards above the character's feet left out. With
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
// blocked, refused, overhead, between, inside, framesInside, draws, group,
// groupFlags, area, doorsOut, parts, sceneryOut, creaturesOut, canChoose,
// canFilter = ARPG_CutawayInfo()
//   asked          1 while a cutaway is asked for
//   cutting        1 while the world is being cut right now
//   height         yards over the character's feet of the cut, 0 when not cutting
//   frames         scene updates since the UI loaded
//   framesCut      how many of them were drawn cut
//   framesSkipped  how many were due a cut but the device or its matrices were not as expected
//   rooms          rooms drawn in the last frame of the inside view (or queued in the last frame cut without it)
//   indoors        1 if the character is in a building: in a room the scene found, or so says the client
//   blocked        1 if, under the open sky, a building stands between camera and character
//   refused        1 if the cut has been given up: Direct3D does not take its clip plane
//   overhead       yards over the feet of the building overhead, 0 if none was found (looked for indoors only)
//   between        yards over the feet of the lowest part of a building between camera and character, 0 if
//                  none (not looked for in the inside view)
//   inside         1 if the character is in a room and the last frame was drawn from it
//   framesInside   frames drawn that way since the UI loaded
//   draws          draws made with the cut in the last frame that had one
//   group          the building group under the character as the scene's lookup has it, -1 if none
//   groupFlags     its flags from the model file, 0 if none
//   area           the area table's flags for it (2 indoors, 4 outdoors), -1 if it has no record
//   doorsOut       in the last frame of the inside view, places beyond a doorway that are not rooms
//   parts          parts of buildings the last cut frame cut for being in the way; -1 if it cut everything
//   sceneryOut     scenery left out of the last frame: in the inside view for standing outside the rooms shown,
//                  outdoors for standing on a part that is cut, wholly above the cut
//   creaturesOut   creatures and objects, likewise
//   canChoose      1 if the library can cut building groups one by one (else the cut under the open sky is for
//                  everything)
//   canFilter      1 if it can leave out scenery and creatures that stand outside the rooms shown
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
    game::lua_pushnumber(L, static_cast<double>(g_stats.draws));
    game::lua_pushnumber(L, g_where.found ? static_cast<double>(g_where.group) : -1.0);
    game::lua_pushnumber(L, static_cast<double>(g_where.flags));
    game::lua_pushnumber(L, g_where.found ? static_cast<double>(g_where.area) : -1.0);
    game::lua_pushnumber(L, static_cast<double>(g_stats.doorsOut));
    game::lua_pushnumber(L, static_cast<double>(g_stats.parts));
    game::lua_pushnumber(L, static_cast<double>(g_stats.sceneryOut));
    game::lua_pushnumber(L, static_cast<double>(g_stats.creaturesOut));
    game::lua_pushnumber(L, flag(g_canChoose));
    game::lua_pushnumber(L, flag(g_canFilter));
    return 24;
}

}  // namespace cutaway
