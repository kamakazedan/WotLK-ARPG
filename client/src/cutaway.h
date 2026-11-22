// The cutaway: seeing the character under a roof. While the character is in
// a building the scene is drawn from the character's room, looked into from
// above, with nothing of the outdoor world, and whatever is higher than a set
// height over the character's feet is cut away. See cutaway.cpp.
#pragma once

#include "game.h"

namespace cutaway {

// Redirects the six calls the cutaway works through. True if all six are in
// place; otherwise the cutaway stays off for good and nothing else is
// affected. A seventh patch, for the fog, is a matter of looks only.
bool Install();

// A fresh UI: off, until the addon asks again.
void Reset();

// ARPG_SetCutaway(height [, outdoors]) / ARPG_SetCutaway()
int __cdecl LuaSetCutaway(game::lua_State* L);

// asked, cutting, height, frames, framesCut, framesSkipped, rooms, indoors,
// blocked, refused, overhead, between, inside, framesInside = ARPG_CutawayInfo()
int __cdecl LuaCutawayInfo(game::lua_State* L);

}  // namespace cutaway
