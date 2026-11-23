// The cutaway: seeing the character under a roof. While the character is in
// a room the scene is drawn from that room, looked into from above, with the
// rooms its doorways lead to and nothing of the outdoor world, and whatever
// is higher than a set height over the character's feet is cut away. See
// cutaway.cpp.
#pragma once

#include "game.h"

namespace cutaway {

// Redirects the nine calls the cutaway works through. True if all nine are
// in place; otherwise the cutaway stays off for good and nothing else is
// affected. Three more let it tell pieces apart (which scenery stands outside
// the rooms shown, which part of a building is in the way); without them it
// works as it did before it could.
bool Install();

// A fresh UI: off, until the addon asks again.
void Reset();

// ARPG_SetCutaway(height [, outdoors]) / ARPG_SetCutaway()
int __cdecl LuaSetCutaway(game::lua_State* L);

// asked, cutting, height, frames, framesCut, framesSkipped, rooms, indoors,
// blocked, refused, overhead, between, inside, framesInside, draws, group,
// groupFlags, area, doorsOut, parts, sceneryOut, creaturesOut, canChoose,
// canFilter = ARPG_CutawayInfo()
int __cdecl LuaCutawayInfo(game::lua_State* L);

}  // namespace cutaway
