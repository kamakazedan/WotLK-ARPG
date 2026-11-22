// The cutaway: with the camera high above the character, whatever is above
// the character's head (a roof, the floor upstairs, a cave's ceiling) is not
// drawn, and the rooms beneath are. See cutaway.cpp.
#pragma once

#include "game.h"

namespace cutaway {

// Redirects the four calls the cutaway works through. True if all four are
// in place; otherwise the cutaway stays off for good and nothing else is
// affected.
bool Install();

// A fresh UI: off, until the addon asks again.
void Reset();

// ARPG_SetCutaway(height [, outdoors]) / ARPG_SetCutaway()
int __cdecl LuaSetCutaway(game::lua_State* L);

// asked, cutting, height, frames, framesCut, framesSkipped, rooms, indoors,
// blocked, refused, overhead, between = ARPG_CutawayInfo()
int __cdecl LuaCutawayInfo(game::lua_State* L);

}  // namespace cutaway
