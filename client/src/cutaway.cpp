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
//      room that is drawn. (Outdoors, a ray that runs in the opening is
//      picked up again where it leaves it.)
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
// Outdoors, when asked, while part of a building hides the character: an
// opening along the line of sight.
//
//   A level cut cannot be made for one building and not the next: the
//   client's files put half a city into one group. And a building cut level
//   shows what is inside it, which is nothing, or rooms. So what is taken
//   out of the buildings here is not a slice but exactly what covers the
//   character on screen, and a strip of screen to either side and above:
//   everything between four planes. Three of them pass through the camera:
//   one down to the line across the view at the character's feet, and an
//   upright one on either side of the character. The fourth stands upright
//   through the character, across the view: what is beyond the character
//   hides nothing of it and is never touched.
//   Because the first three pass through the camera, the camera sees them
//   edge on. Where they cut a wall there is no cut face to look at and no
//   way to look into the building: of it there is on screen either its own
//   outside, as ever, or nothing. The fourth plane is seen face on, and a
//   building that stands on both sides of it within the opening (the
//   archway the character is under, a house right beside it) shows its cut
//   end there. So the opening is no wider than the free space beside the
//   character: each side of it stops short of the nearest part of a
//   building found beside the character, knee-high, to the left and to the
//   right.
//   A group that reaches into the opening is drawn once for each side of it
//   that stays (below it, beyond the character, to the left, to the right),
//   each time with the clip planes for that side; what lies in the opening
//   is in none of them. Groups that do not reach into it are drawn as ever,
//   and so are the terrain and everything else. Models are not cut: the
//   client draws many of a kind in a single draw. Scenery and creatures
//   that are wholly in the opening are left out.
//
// Where the level cut is made:
//   - Under a roof lower than the camera: at a set height over the
//     character's feet. Under a ceiling lower than that, just under the
//     ceiling. In a tall room, just under whatever is in the way.
//   - Under a roof higher than the camera, seen from inside: not at all.
//   - Under the open sky: never. There it is the opening, if asked for,
//     while part of a building stands between the camera and the character.
// The plane slides down from the camera when a cut begins and back up when
// it ends; the opening does the same, its lower edge coming down the screen
// to the character's feet. In between, the level cut comes down at once and
// goes back up only after a pause longer than a jump, so that neither
// jumping nor walking under a row of beams makes it bob. The opening stays
// with the feet. Neither outlives the place it was made for: a level cut is
// gone the moment the character is under the open sky, and an opening the
// moment it is not.
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
constexpr uint32_t kClipPlane = 5;       // which of Direct3D's six user clip planes the level cut uses: the last
                                         // one (the client itself uses the first, for a few models)
constexpr uint32_t kClipPlaneBit = 1u << kClipPlane;
constexpr int kWindowPlanes = 3;         // the opening outdoors uses the last three: 5, 4 and 3
constexpr int kWindowPasses = 4;         // and draws a group once for each side of it that stays
constexpr float kFootRoom = 0.25f;       // yards over the feet the opening comes down to: the ground the character
                                         // stands on is never in it
constexpr float kWindowHalf = 7.0f;      // yards to either side of the character the opening is wide, at most
constexpr float kMinHalf = 1.5f;         // and at least: the character is seen whole
constexpr float kSideClear = 0.5f;       // yards its sides keep clear of a building that stands beside the character
constexpr float kSideProbe = 0.6f;       // yards over the feet at which that is looked for: low enough to find a
                                         // flight of steps or a raised pavement, not so low as to find a kerb
constexpr float kNarrowSpeed = 30.0f;    // yards per second a side of it comes in when it has to
constexpr float kWidenSpeed = 3.0f;      // and goes out again: slowly, so that it does not twitch at every doorway
                                         // and between every two pillars the character walks past
constexpr float kSeam = 0.02f;           // yards the sides of the opening that meet at a plane are drawn into each
                                         // other, so that no line of the screen falls between two of them
constexpr float kMinSideways = 0.5f;     // yards the camera has to be to one side of the character for there to be
                                         // a "between the two"
constexpr float kBehindRooms = 2.0f;     // yards behind the farthest corner of the rooms the background is put
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
    bool outdoors;  // outdoors, open up what stands between camera and character
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
    bool watching;      // this frame's picture is the cutaway's business: switched on, in the world, character found
    float overhead;     // yards over the feet of what is overhead, 0 if nothing was found (looked for indoors only)
    float between;      // yards over the feet of the lowest thing between camera and character, 0 if nothing
                        // (not looked for in the inside view, where walls do not hide what is behind them)
    bool blocked;       // in the open, with the view of the character blocked for long enough
    bool blockPending;
    uint32_t blockedSince;
    uint32_t lastBlockedTick;
    bool rising;        // the plane could go up
    uint32_t risingSince;
    bool open;          // the character is under the open sky: a cut that begins there is an opening along the
                        // line of sight
    bool opening;       // the cut in progress is one
    float sideLeft;     // yards to the left and to the right of the character it reaches at the moment
    float sideRight;
    bool drawnCut;      // the last frame that asked for a cut was drawn with one

    bool givenUp;       // Direct3D does not take the cut's clip plane
    uint32_t refusals;
};
State g_state = {};

// A plane of the world. A point's value is its distance from the plane in
// yards, positive on one side.
struct Plane {
    Vec3 normal;  // of length 1
    float offset;
};

// Outdoors, the opening along the line of sight: what lies on the positive
// side of all four planes is taken out of the buildings.
struct Window {
    bool on;       // this frame's cut is such an opening
    Plane under;   // through the camera and the line across the view at the character's feet: positive above it
    Plane behind;  // upright through the character, across the view: positive on the camera's side
    Plane left;    // upright through the camera and a point to one side of the character: positive on the
                   // character's side
    Plane right;   // the same on the other side
};
Window g_window = {};

// One of the times a group that reaches into the opening is drawn: with
// these clip planes, for one side of the opening that stays.
struct Pass {
    uint32_t planes;               // Direct3D's switch for it
    float clip[kWindowPlanes][4];  // for planes 5, 4 and 3, as many as are on
};

// The clip planes on the graphics device, for one frame's drawing of the world.
struct Cut {
    bool on;                  // from just before the world is drawn until it has been
    bool sent;                // Direct3D's clip-plane switch has this library's bits in it
    bool all;                 // every draw of the world is cut, at the level plane; otherwise building groups
                              // only, by the opening...
    uint32_t planes;          // ... and these are the planes on for the draw in hand: none, outside a group's passes
    game::Matrix projection;  // the hardware projection the planes were worked out for
    Pass passes[kWindowPasses];
};
Cut g_cut = {};

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
    int32_t parts;           // groups of buildings that frame drew with a piece taken out for the opening;
                             // -1 if its cut was the level one, for everything
    uint32_t sceneryOut;     // scenery left out of the last frame: for standing outside the rooms shown, or
                             // outdoors in the opening
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

float Above(const Plane& plane, const Vec3& point) {
    return plane.normal.x * point.x + plane.normal.y * point.y + plane.normal.z * point.z + plane.offset;
}

// The plane with this normal, of any length but none, through a point.
Plane Through(const Vec3& normal, const Vec3& point) {
    const float length = Sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
    const Vec3 unit = {normal.x / length, normal.y / length, normal.z / length};
    return {unit, -(unit.x * point.x + unit.y * point.y + unit.z * point.z)};
}

// The opening along the line of sight, for a camera, a character, the
// height at the character that its lower edge has come down to, and how far
// it reaches to either side of the character. False if the camera is
// straight over the character: there is no "between the two" then.
bool SetWindow(const Vec3& feet, const Vec3& eye, float edgeZ, float halfLeft, float halfRight) {
    const float x = feet.x - eye.x;
    const float y = feet.y - eye.y;
    const float sideways = Sqrt(x * x + y * y);
    const float down = eye.z - edgeZ;
    if (!(sideways >= kMinSideways) || !(down > 0.0f)) {
        return false;
    }
    // Level and of length 1: from the camera towards the character, and to
    // the right of that.
    const Vec3 forward = {x / sideways, y / sideways, 0.0f};
    const Vec3 right = {forward.y, -forward.x, 0.0f};
    // Through the camera and the level line across the view at (feet,
    // edgeZ). Over that line, a yard up is `sideways` further along this
    // normal.
    g_window.under = Through({down * forward.x, down * forward.y, sideways}, eye);
    g_window.behind = Through({-forward.x, -forward.y, 0.0f}, feet);
    // Upright, through the camera and a point level with it to either side
    // of the character.
    const Vec3 toLeft = {x - halfLeft * right.x, y - halfLeft * right.y, 0.0f};
    const Vec3 toRight = {x + halfRight * right.x, y + halfRight * right.y, 0.0f};
    g_window.left = Through({toLeft.y, -toLeft.x, 0.0f}, eye);
    g_window.right = Through({-toRight.y, toRight.x, 0.0f}, eye);
    return true;
}

// How far it is, level, from a point to the nearest part of a building in a
// direction (of length 1), as far as `reach`. The line is tried from both
// ends, like the line of sight, and a hit is believed only if it lies where
// the routine's own "how far along" says it does, measured from either end.
float ClearTowards(const Vec3& from, const Vec3& direction, float reach) {
    const Vec3 to = {from.x + direction.x * reach, from.y + direction.y * reach, from.z};
    float clear = reach;
    for (int back = 0; back < 2; ++back) {
        Vec3 hit = {0.0f, 0.0f, 0.0f};
        float fraction = 1.0f;
        if (!game::IntersectWorld(back != 0 ? to : from, back != 0 ? from : to, &hit, &fraction, game::kIntersectBuildings) ||
            !(fraction >= 0.0f && fraction <= 1.0f)) {
            continue;
        }
        const float away = (hit.x - from.x) * direction.x + (hit.y - from.y) * direction.y;
        if (Abs(away - fraction * reach) > 0.5f && Abs(away - (1.0f - fraction) * reach) > 0.5f) {
            continue;
        }
        clear = Min(clear, Max(away, 0.0f));
    }
    return clear;
}

// How far the opening reaches to either side of the character: as far as
// nothing of a building stands beside the character, knee-high or higher,
// less a little, and within limits. What stands beside the character hides
// nothing of it. And a building that the opening took a piece out of on the
// camera's side of the character would show its cut end on the other side,
// where it goes on: the one place where an opening can be looked into. That
// goes for a house wall and as much for steps or a raised pavement, which
// the plane under the opening, so close over the ground there, would take a
// strip out of.
// A side comes in fast and goes out slowly, and an opening begins as wide as
// it may be.
void SetSides(const Vec3& feet, const Vec3& eye, float elapsed, bool begun) {
    const float x = feet.x - eye.x;
    const float y = feet.y - eye.y;
    const float sideways = Sqrt(x * x + y * y);
    if (!(sideways >= kMinSideways)) {
        return;
    }
    const Vec3 right = {y / sideways, -x / sideways, 0.0f};
    const Vec3 left = {-right.x, -right.y, 0.0f};
    const Vec3 from = {feet.x, feet.y, feet.z + kSideProbe};
    const float reach = kWindowHalf + kSideClear;
    const float wanted[2] = {Max(kMinHalf, ClearTowards(from, left, reach) - kSideClear),
                             Max(kMinHalf, ClearTowards(from, right, reach) - kSideClear)};
    float* const sides[2] = {&g_state.sideLeft, &g_state.sideRight};
    for (int i = 0; i < 2; ++i) {
        float& side = *sides[i];
        if (begun) {
            side = wanted[i];
        } else if (wanted[i] < side) {
            side = Max(wanted[i], side - kNarrowSpeed * elapsed);
        } else {
            side = Min(wanted[i], side + kWidenSpeed * elapsed);
        }
    }
}

// Once per frame, from the scene update: decides whether this frame is cut,
// which way and how far. `inRoom`: the character is in a room, and the frame
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
    const bool asked = g_settings.outdoors && g_canChoose;  // for the opening under the open sky
    const bool sight = !inRoom && (g_state.indoors || asked) && LowestBetween(chest, eye, &sightZ);
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
        g_window.on = false;
        return;
    }

    // The cut keeps clear of the character, and a jump takes the cut up with
    // it only for as far as that needs.
    const float room = feet.z + g_settings.height;
    float limit = feet.z + Min(g_settings.height, kHeadroom);

    bool wanted = false;
    float target = 0.0f;
    g_state.open = false;
    g_window.on = false;
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
        // lookup did not find). No level cut here: an opening along the
        // line of sight, down to the character's feet. It is taken out of
        // buildings only, so there is nothing to keep clear of.
        g_state.open = true;
        if (sight && asked) {
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
            if (!asked || now - g_state.lastBlockedTick >= kClearDelayMs) {
                g_state.blocked = false;
            }
        }
        wanted = g_state.blocked;
        target = feet.z + kFootRoom;
        limit = target;  // (which also keeps its edge with the feet when they go up: it is never under the limit)
    }

    // A cut is made for the place the character is in and does not outlive
    // it: under the open sky there is never a level cut, not even one on its
    // way out across the street, and under a roof never an opening.
    if (g_state.cutting && g_state.opening != g_state.open) {
        g_state.cutting = false;
    }

    const float top = eye.z - kEyeMargin;
    if (wanted) {
        target = Max(target, limit);
        if (target >= top) {
            wanted = false;  // the camera is too low for there to be anything to cut
        }
    }

    bool begun = false;
    if (wanted) {
        if (!g_state.cutting) {
            g_state.cutting = true;
            g_state.opening = g_state.open;
            g_state.rising = false;
            g_state.planeZ = top;
            begun = true;
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
    if (g_state.active && g_state.opening) {
        SetSides(feet, eye, elapsed, begun);
        g_window.on = SetWindow(feet, eye, g_state.planeZ, g_state.sideLeft, g_state.sideRight);
        g_state.active = g_window.on;
    }
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

// The fog of a building is for whoever is in the building. The client asks
// for it by where the camera is: which building, which of its groups, how
// deep in. A camera that does not stop at walls hangs in an attic or over a
// hall while the character is in the street, and gets the fog of that
// place: thick, and ending a few dozen yards off, and the client leaves
// out what is further away than a fog's end. From high enough up that is
// the far side of the street, or all of it and the character too. So
// unless the frame is drawn from the room the character is in, there is no
// building's fog at all: the answer the client's own routine gives for a
// camera in no building.
using BuildingFog = int (__cdecl*)(void* fog, void** building, uint8_t* inside, void** groups, float* depth);
BuildingFog g_originalBuildingFog = nullptr;

int __cdecl BuildingFogHook(void* fog, void** building, uint8_t* inside, void** groups, float* depth) {
    if (g_state.watching && !g_state.interior) {
        return 0;
    }
    return g_originalBuildingFog(fog, building, inside, groups, depth);
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
        g_state.watching = false;
        g_window.on = false;
        g_state.overhead = 0.0f;
        g_state.between = 0.0f;
        g_where = {};
        g_originalLocateCamera();
        return;
    }
    g_state.watching = true;

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
}

// ---------------------------------------------------------------------------
// Outdoors: the opening along the line of sight
// ---------------------------------------------------------------------------

// The most and the least a plane makes of any point of a box.
float Most(const Plane& plane, const game::Box& box) {
    const Vec3 corner = {plane.normal.x >= 0.0f ? box.high.x : box.low.x, plane.normal.y >= 0.0f ? box.high.y : box.low.y,
                         plane.normal.z >= 0.0f ? box.high.z : box.low.z};
    return Above(plane, corner);
}

float Least(const Plane& plane, const game::Box& box) {
    const Vec3 corner = {plane.normal.x >= 0.0f ? box.low.x : box.high.x, plane.normal.y >= 0.0f ? box.low.y : box.high.y,
                         plane.normal.z >= 0.0f ? box.low.z : box.high.z};
    return Above(plane, corner);
}

// Whether a box may reach into the opening. (Yes for some that do not: a
// box is judged by one plane at a time. Those are drawn in passes like the
// rest, and come out whole.)
bool ReachesIntoWindow(const game::Box& box) {
    return Most(g_window.under, box) > 0.0f && Most(g_window.behind, box) > 0.0f && Most(g_window.left, box) > 0.0f &&
           Most(g_window.right, box) > 0.0f;
}

// The sides of the opening that stay, one pass each: below it; above that
// and beyond the character; above it, on the camera's side, and to the left
// of it; the same to the right. Between them they are everything but the
// opening, and no two of them share more than the plane between them (but
// behind the camera, where nothing is drawn). Whether a box that reaches
// into the opening has anything in such a side.
bool PassShows(int pass, const game::Box& box) {
    if (pass == 0) {
        return Least(g_window.under, box) < 0.0f;
    }
    if (pass == 1) {
        return Least(g_window.behind, box) < 0.0f;
    }
    return Most(g_window.behind, box) > 0.0f && Least(pass == 2 ? g_window.left : g_window.right, box) < 0.0f;
}

// Whether a ball is wholly in the opening.
bool WhollyInWindow(const Vec3& centre, float radius) {
    return Above(g_window.under, centre) > radius && Above(g_window.behind, centre) > radius &&
           Above(g_window.left, centre) > radius && Above(g_window.right, centre) > radius;
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

// The level cut without the inside view: every indoor group of a building
// that is under the cut and in view.
void ShowRoomsOf(uint8_t* building) {
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

// The level cut without the inside view shows the rooms it slices into. The
// opening outdoors shows none: nobody can look into a building through it.
void __cdecl FinishVisitsHook() {
    if (g_state.active && g_state.opening) {
        g_stats.rooms = 0;
    } else if (g_state.active && !g_state.interior) {
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

// Outdoors, in the opening along the line of sight. A model cannot be cut:
// the client draws many of a kind in a single draw (0x00820AE0), and a clip
// plane is on or off for all of it. So what is wholly in the opening is left
// out: the sign over the door, the chimney, whoever stands on the roof.
// What reaches out of it is drawn whole. And nothing is left out unless the
// opening is really being made: whether this frame's will be is not known
// yet when the scene asks (the device may not be one this library draws on,
// or its matrices not what they are taken to be), so the frame before
// stands in for it.
bool InTheOpening(const Vec3& centre, float radius) {
    return g_state.drawnCut && g_window.on && WhollyInWindow(centre, radius);
}

// A piece of scenery or a creature shows in the view of a group being gone
// through for what is in it. Whether it is drawn: in the inside view, only
// if it stands in a room that is shown; outdoors, unless it is in the
// opening.
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
        keep = !InTheOpening(*centre, radius);
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

// A plane of the world as Direct3D takes a user clip plane while a vertex
// shader is in use: in clip space, and positive where things are drawn,
// which is the plane's own positive side for `side` 1 and the other for -1.
//
// A point p of the world is (p - eye) * view in view space, so there the
// plane has its normal rotated by `view` and, as its offset, its value at
// the camera. A view-space point (x, y, z, 1) leaves a vertex shader as
// (x * m00, y * m11, z * m22 + m32, z), with m the hardware projection;
// dotted with the four numbers made here, that is the view-space plane's
// value at the point again. `seam`: yards past the plane that are drawn too.
void ClipPlane(const Plane& plane, float side, float seam, const Matrix& view, const Matrix& hardware, float clip[4]) {
    float normal[3];
    for (int j = 0; j < 3; ++j) {
        normal[j] = side * (plane.normal.x * view.m[0][j] + plane.normal.y * view.m[1][j] + plane.normal.z * view.m[2][j]);
    }
    const float atEye = side * Above(plane, g_state.eye) + seam;
    clip[0] = normal[0] / hardware.m[0][0];
    clip[1] = normal[1] / hardware.m[1][1];
    clip[2] = atEye / hardware.m[3][2];
    clip[3] = normal[2] - hardware.m[2][2] * atEye / hardware.m[3][2];
}

// Gives Direct3D the clip planes of one pass. False if it refuses one.
bool SendPass(void* direct3d, const Pass& pass) {
    for (int i = 0; i < kWindowPlanes; ++i) {
        if ((pass.planes & (kClipPlaneBit >> i)) != 0 &&
            game::Direct3DSetClipPlane(direct3d, kClipPlane - static_cast<uint32_t>(i), pass.clip[i]) < 0) {
            return false;
        }
    }
    return true;
}

// Works out the cut's clip planes and asks Direct3D whether it takes them.
// Runs once the client has decided what is in view, just before it draws;
// the planes are then switched on draw by draw (below) until the world has
// been drawn.
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

    bool taken = true;
    if (!g_window.on) {
        // The level cut: what is below planeZ stays. The camera is above it.
        const Plane level = {{0.0f, 0.0f, 1.0f}, -g_state.planeZ};
        if (!(Above(level, eye) > 0.0f)) {
            Skip(kSkipPlane, Thousandths(-Above(level, eye)), 0, 0);
            return;
        }
        float clip[4];
        ClipPlane(level, -1.0f, 0.0f, view, hardware, clip);
        taken = game::Direct3DSetClipPlane(direct3d, kClipPlane, clip) >= 0;
    } else {
        // The opening: the sides of it that stay, each the positive side of
        // up to three planes. The side below the opening and the side beyond
        // the character are drawn a hair past the plane where the others
        // begin: two draws that only met at a plane could leave single
        // points of the screen to neither, all along it.
        const Plane* const planes[kWindowPasses][kWindowPlanes] = {
            {&g_window.under, nullptr, nullptr},
            {&g_window.under, &g_window.behind, nullptr},
            {&g_window.under, &g_window.behind, &g_window.left},
            {&g_window.under, &g_window.behind, &g_window.right},
        };
        const float sides[kWindowPasses][kWindowPlanes] = {
            {-1.0f, 0.0f, 0.0f}, {1.0f, -1.0f, 0.0f}, {1.0f, 1.0f, -1.0f}, {1.0f, 1.0f, -1.0f}};
        const float seams[kWindowPasses][kWindowPlanes] = {
            {kSeam, 0.0f, 0.0f}, {0.0f, kSeam, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
        for (int pass = 0; pass < kWindowPasses; ++pass) {
            g_cut.passes[pass].planes = 0;
            for (int i = 0; i < kWindowPlanes && planes[pass][i] != nullptr; ++i) {
                ClipPlane(*planes[pass][i], sides[pass][i], seams[pass][i], view, hardware, g_cut.passes[pass].clip[i]);
                g_cut.passes[pass].planes |= kClipPlaneBit >> i;
            }
        }
        // Asked now whether Direct3D takes all three, rather than in the
        // middle of the drawing.
        taken = SendPass(direct3d, g_cut.passes[kWindowPasses - 1]);
    }
    if (!taken) {
        if (!g_stats.loggedRefusal) {
            g_stats.loggedRefusal = true;
            logfile::Write("Cutaway: Direct3D does not take user clip plane %d%s. No cut.", static_cast<int>(kClipPlane),
                           g_window.on ? ", 4 or 3" : "");
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
    g_cut.all = !g_window.on;
    g_cut.planes = 0;
    g_cut.projection = hardware;
    g_stats.draws = 0;
    g_stats.parts = g_cut.all ? -1 : 0;
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
// While the world is being drawn cut, the level plane is switched on for a
// draw that is made with a vertex shader and with the world's projection:
// those are the draws the plane is right for. For any other it is switched
// off. Where the cut is the opening outdoors, the planes on are those of the
// pass in hand, and none outside a group's passes.
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
    uint32_t planes = !g_cut.on ? 0 : (g_cut.all ? kClipPlaneBit : g_cut.planes);
    if (planes != 0) {
        uint8_t* states = At<uint8_t*>(device, game::kGxDeviceStates);
        if (states == nullptr || At<void*>(states, game::kGxStateVertexShader * game::kGxStateSize) == nullptr ||
            !SameMatrix(At<Matrix>(device, game::kGxDeviceHardwareProjection), g_cut.projection)) {
            planes = 0;
        }
    }
    if (planes != 0) {
        game::Direct3DSetRenderState(direct3d, game::kDirect3DClipPlaneEnable, ClientsClipPlanes(device) | planes);
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

// One group of a building is drawn. Where the cut is the opening outdoors
// and the group's box reaches into it, the group is drawn once for each side
// of the opening that stays and that its box has anything in, with that
// side's clip planes: so, of it, everything is drawn but what is in the
// opening. Rooms the client has found through a doorway are groups like any
// other here.
using DrawGroup = void (__thiscall*)(uint8_t* model, uint32_t group, uint8_t* placement, void* views);
DrawGroup g_originalDrawGroup = nullptr;

// The box of a group about to be drawn, if what is passed for its building's
// matrix does belong to a placed building of this model.
const game::Box* BoxOfGroup(uint8_t* model, uint32_t group, uint8_t* placement) {
    if (model == nullptr || placement == nullptr) {
        return nullptr;
    }
    uint8_t* building = placement - game::kBuildingPlacement;
    if (ReadyModel(building) != model) {
        return nullptr;
    }
    const uint8_t* placed = static_cast<const uint8_t*>(PlacedGroupAt(building, group));
    return placed != nullptr ? reinterpret_cast<const game::Box*>(placed + game::kPlacedGroupBounds) : nullptr;
}

void __fastcall DrawGroupHook(uint8_t* model, void* /*edx*/, uint32_t group, uint8_t* placement, void* views) {
    void* direct3d = nullptr;
    const game::Box* box = g_cut.on && !g_cut.all ? BoxOfGroup(model, group, placement) : nullptr;
    if (box == nullptr || !ReachesIntoWindow(*box) || Direct3DDevice(&direct3d) == nullptr) {
        g_originalDrawGroup(model, group, placement, views);
        return;
    }
    ++g_stats.parts;
    for (int pass = 0; pass < kWindowPasses; ++pass) {
        if (PassShows(pass, *box) && SendPass(direct3d, g_cut.passes[pass])) {
            g_cut.planes = g_cut.passes[pass].planes;
            g_originalDrawGroup(model, group, placement, views);
        }
    }
    g_cut.planes = 0;
}

Plain g_originalBeforeDrawing = nullptr;

// The scene has found what is in view and is about to draw it.
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

// Outdoors, what is in the opening is not on screen, and must not catch a
// click on what is seen through it. The cursor's ray starts at the camera,
// which all the opening's planes but one pass through: so a ray that sets
// out above the plane under the opening and between its sides runs in the
// opening all the way to the upright plane through the character, and is
// picked up again there. Any other ray runs in it nowhere.
void LeaveWindow(Vec3* start, const Vec3& end) {
    const Vec3 way = {end.x - start->x, end.y - start->y, end.z - start->z};
    const auto along = [&way](const Plane& plane) {
        return plane.normal.x * way.x + plane.normal.y * way.y + plane.normal.z * way.z;
    };
    if (!(along(g_window.under) > 0.0f && along(g_window.left) > 0.0f && along(g_window.right) > 0.0f)) {
        return;
    }
    const float before = Above(g_window.behind, *start);
    const float after = Above(g_window.behind, end);
    if (!(before > 0.0f && after < 0.0f)) {
        return;
    }
    const float share = before / (before - after);
    *start = {start->x + way.x * share, start->y + way.y * share, start->z + way.z * share};
}

using CursorRay = int (__thiscall*)(uint8_t* worldFrame, float x, float y, Vec3* start, Vec3* end);
CursorRay g_originalCursorRay = nullptr;

int __fastcall CursorRayHook(uint8_t* worldFrame, void* /*edx*/, float x, float y, Vec3* start, Vec3* end) {
    const int found = g_originalCursorRay(worldFrame, x, y, start, end);
    if (found == 0 || start == nullptr || end == nullptr || !g_installed || !g_settings.enabled) {
        return found;
    }
    if (g_state.cutting && g_state.opening) {
        if (g_window.on && g_state.drawnCut) {
            LeaveWindow(start, *end);
        }
    } else if (g_state.cutting && start->z > g_state.planeZ && end->z < g_state.planeZ) {
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
    const hook::Site fog = {"Cutaway: fog of the camera's building", game::kBuildingFogCall,
                            game::kBuildingFogCallBytes, sizeof(game::kBuildingFogCallBytes), 0};

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
    // drawn, nothing is opened up under the open sky. Either way the rest
    // works as it did.
    bool filter = true;
    filter &= hook::IsActive(hook::RedirectCall(scenery, reinterpret_cast<void*>(SceneryInViewHook),
                                                reinterpret_cast<void**>(&g_originalSceneryInView)));
    filter &= hook::IsActive(hook::RedirectCall(creatures, reinterpret_cast<void*>(CreatureInViewHook),
                                                reinterpret_cast<void**>(&g_originalCreatureInView)));
    const bool choose = hook::IsActive(hook::RedirectCall(group, reinterpret_cast<void*>(DrawGroupHook),
                                                          reinterpret_cast<void**>(&g_originalDrawGroup)));
    g_canFilter = all && filter;
    g_canChoose = all && filter && choose;

    // And one more, apart from those: without it the fog is that of
    // wherever the camera hangs.
    if (!hook::IsActive(hook::RedirectCall(fog, reinterpret_cast<void*>(BuildingFogHook),
                                           reinterpret_cast<void**>(&g_originalBuildingFog))) &&
        all) {
        logfile::Write("Cutaway: the fog stays that of the building the camera is in, wherever the character is");
    }
    if (all && !g_canChoose) {
        logfile::Write("Cutaway: %s", g_canFilter ? "cannot tell building groups apart as they are drawn; nothing "
                                                    "is opened up under the open sky"
                                                  : "cannot tell where scenery and creatures stand; they are drawn "
                                                    "wherever the game files them, and nothing is opened up under "
                                                    "the open sky");
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
    g_window = {};
    g_cut.planes = 0;
}

// ARPG_SetCutaway(height [, outdoors]): while the character is in a room,
// show the building from inside and nothing else, with whatever is more than
// `height` yards above the character's feet left out. With outdoors true,
// also open up, under the open sky, whatever of a building stands between
// the camera and the character. ARPG_SetCutaway() switches it all off.
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
// canFilter, openLeft, openRight = ARPG_CutawayInfo()
//   asked          1 while a cutaway is asked for
//   cutting        1 while the world is being cut right now
//   height         yards over the character's feet of the cut, 0 when not cutting; for the opening outdoors, of
//                  its lower edge
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
//   parts          groups of buildings the last cut frame drew with a piece taken out for the opening outdoors;
//                  -1 if its cut was the level one, for everything
//   sceneryOut     scenery left out of the last frame: in the inside view for standing outside the rooms shown,
//                  outdoors for standing in the opening
//   creaturesOut   creatures and objects, likewise
//   canChoose      1 if the library can draw building groups in passes (else nothing is opened outdoors)
//   canFilter      1 if it can leave out scenery and creatures that stand outside the rooms shown
//   openLeft       yards to the left of the character the opening outdoors reaches, 0 while there is none
//   openRight      and to the right
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
    const bool opening = g_state.cutting && g_state.opening;
    game::lua_pushnumber(L, opening ? static_cast<double>(g_state.sideLeft) : 0.0);
    game::lua_pushnumber(L, opening ? static_cast<double>(g_state.sideRight) : 0.0);
    return 26;
}

}  // namespace cutaway
