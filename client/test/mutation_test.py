#!/usr/bin/env python3
"""Breaks the cutaway on purpose, one rule at a time, and checks that the
test suite notices every time. A breakage the tests let through is a hole in
the tests, not in the library.

    python3 client/test/mutation_test.py            all of them (a few minutes)
    python3 client/test/mutation_test.py halved     only those with "halved" in the name
    python3 client/test/mutation_test.py --jobs 2   fewer at once (default 4)

Each breakage is a piece of src/cutaway.cpp swapped for another. It is made
in a scratch copy of client/, never in the working tree, and run through
test/run.sh there. When the source changes, the text of a breakage may no
longer be found; the script says so, and the breakage wants updating.
"""
import concurrent.futures
import os
import shutil
import subprocess
import sys
import tempfile

CLIENT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FILE = os.path.join("src", "cutaway.cpp")

# (what is broken, [(text in cutaway.cpp, what replaces it), ...])
MUTATIONS = [
    # When and where
    ("a hall higher than the camera is cut anyway",
     [("const bool enclosed = g_state.indoors && !(roof && roofZ >= eye.z);",
       "const bool enclosed = g_state.indoors && !(roof && roofZ >= eye.z + 1000.0f);")]),
    ("no room is ever a tall one",
     [("if (lowest > room + kTallRoom) {", "if (lowest > room + kTallRoom + 1000.0f) {")]),
    ("a tall room is cut above its ceiling",
     [("target = lowest - kBlockMargin;  // a tall room: only as much as is in the way", "target = lowest + 2.0f;")]),
    ("a low ceiling is left in as a lid",
     [("if (roof && roofZ - kBlockMargin < room) {", "if (roof && roofZ - kBlockMargin < room - 1000.0f) {")]),
    ("under a very low ceiling the cut keeps its usual distance",
     [("            limit = Min(limit, Max(target, feet.z + kMinHeight));\n", "")]),
    ("under a very low ceiling the cut comes down onto the character",
     [("limit = Min(limit, Max(target, feet.z + kMinHeight));",
       "limit = Min(limit, Max(target, feet.z + kMinHeight - 1.5f));")]),
    ("the higher of roof and obstruction is taken",
     [("const float lowest = sight && roof ? Min(sightZ, roofZ) : (sight ? sightZ : roofZ);",
       "const float lowest = sight && roof ? Max(sightZ, roofZ) : (sight ? sightZ : roofZ);")]),
    ("indoors the line of sight is only looked at when the outdoor part is asked for",
     [("const bool sight = !inRoom && (g_state.indoors || g_settings.outdoors) && LowestBetween(chest, eye, &sightZ);",
       "const bool sight = !inRoom && g_settings.outdoors && LowestBetween(chest, eye, &sightZ);")]),
    ("nobody looks up for the roof",
     [("const bool roof = g_state.indoors && LowestBetween(waist, sky, &roofZ);",
       "const bool roof = g_state.indoors && false && LowestBetween(waist, sky, &roofZ);")]),
    ("in a hall, a block counts without being asked for",
     [("if (sight && g_settings.outdoors) {", "if (sight) {")]),
    ("the cut in the open is made above what blocks",
     [("        target = g_state.blockZ - kBlockMargin;\n    }", "        target = g_state.blockZ + 2.0f;\n    }")]),
    ("a camera too low still cuts",
     [("            wanted = false;  // the camera is too low for there to be anything to cut",
       "            wanted = wanted && true;")]),
    ("the character is not kept whole while the plane holds still",
     [("g_state.planeZ = Max(plane, limit);  // whatever else, the character stays whole", "g_state.planeZ = plane;")]),

    # Timing
    ("the plane goes back up without a pause",
     [("if (now - g_state.risingSince >= kRiseDelayMs) {", "if (now - g_state.risingSince >= kRiseDelayMs * 0) {")]),
    ("the pause is not started afresh when something gets in the way again",
     [("            g_state.rising = false;\n            plane = Max(target, plane - kSlideSpeed * elapsed);",
       "            plane = Max(target, plane - kSlideSpeed * elapsed);")]),
    ("the plane never goes back up",
     [("plane = Min(target, plane + kRiseSpeed * elapsed);",
       "plane = Min(target, plane + kRiseSpeed * 0.0f * elapsed);")]),
    ("the clear delay is gone",
     [("if (!g_settings.outdoors || now - g_state.lastBlockedTick >= kClearDelayMs) {",
       "if (!g_settings.outdoors || now - g_state.lastBlockedTick >= kClearDelayMs * 0) {")]),
    ("the block delay is gone",
     [("if (now - g_state.blockedSince >= kBlockDelayMs) {", "if (now - g_state.blockedSince >= kBlockDelayMs * 0) {")]),

    # The two lines through the world
    ("the upward hit is taken whatever the downward one says",
     [("    *lowest = hitUp && hitDown ? Min(up, down) : (hitUp ? up : down);\n    return true;",
       "    *lowest = hitUp ? up : down;\n    return true;")]),
    ("the downward hit is taken whatever the upward one says",
     [("    *lowest = hitUp && hitDown ? Min(up, down) : (hitUp ? up : down);\n    return true;",
       "    *lowest = hitDown ? down : up;\n    return true;")]),
    ("the look upwards starts under the floor",
     [("const Vec3 waist = {feet.x, feet.y, feet.z + kWaist};",
       "const Vec3 waist = {feet.x, feet.y, feet.z + kWaist - 50.0f};")]),
    ("the line of sight starts under the floor",
     [("    // from the camera's side, so there the line of sight is not asked about.\n    const Vec3 chest = {feet.x, feet.y, feet.z + kChest};",
       "    // from the camera's side, so there the line of sight is not asked about.\n    const Vec3 chest = {feet.x, feet.y, feet.z + kChest - 3.0f};")]),
    ("the line of sight starts at the knees, where a low wall is in the way",
     [("    // from the camera's side, so there the line of sight is not asked about.\n    const Vec3 chest = {feet.x, feet.y, feet.z + kChest};",
       "    // from the camera's side, so there the line of sight is not asked about.\n    const Vec3 chest = {feet.x, feet.y, feet.z + kChest - 1.0f};")]),
    ("a hit that contradicts itself is believed",
     [("    if (Abs(hit.z - fromStart) > 0.5f && Abs(hit.z - fromEnd) > 0.5f) {",
       "    if (Abs(hit.z - fromStart) > 1000.0f && Abs(hit.z - fromEnd) > 1000.0f) {")]),
    ("the two heights in the status are the wrong way round",
     [("    game::lua_pushnumber(L, static_cast<double>(g_state.overhead));\n"
       "    game::lua_pushnumber(L, static_cast<double>(g_state.between));",
       "    game::lua_pushnumber(L, static_cast<double>(g_state.between));\n"
       "    game::lua_pushnumber(L, static_cast<double>(g_state.overhead));")]),

    # Which room
    ("the room is looked for from under the floor",
     [("    game::ObjectPosition(player, &feet);\n    const Vec3 chest = {feet.x, feet.y, feet.z + kChest};",
       "    game::ObjectPosition(player, &feet);\n    const Vec3 chest = {feet.x, feet.y, feet.z + kChest - 3.0f};")]),
    ("the room is looked for under the camera, not under the character",
     [("        *camera = chest;\n", "")]),
    ("the camera's position is not put back",
     [("        g_originalLocateCamera();\n        *camera = eye;\n", "        g_originalLocateCamera();\n")]),
    ("what the client's own routine made of the room is not looked at",
     [("inRoom = *reinterpret_cast<uint8_t* const*>(game::kSceneCameraBuilding) == room.building;",
       "inRoom = room.building != nullptr;")]),
    ("the area table's 'outdoors' is not heard: a courtyard is a room",
     [("        if ((flags & game::kAreaFlagOutdoors) != 0) {\n            outdoors = true;",
       "        if ((flags & game::kAreaFlagOutdoors) != 0 && false) {\n            outdoors = true;")]),
    ("the area table's 'indoors' is not heard",
     [("        } else if ((flags & game::kAreaFlagIndoors) != 0) {",
       "        } else if ((flags & game::kAreaFlagIndoors) != 0 && false) {")]),
    ("the area table is asked with the wrong name set",
     [("At<uint32_t>(building, game::kBuildingNameSet),", "At<uint32_t>(building, game::kBuildingNameSet) + 1,")]),
    ("the area table is asked without the model's number",
     [("game::FindBuildingArea(Read<uint32_t>(header, game::kModelHeaderId),",
       "game::FindBuildingArea(Read<uint32_t>(header, game::kModelHeaderId) * 0,")]),
    ("the area table is asked by the group's index, not its number",
     [("                                                      At<uint32_t>(data, game::kGroupDataAreaId));",
       "                                                      At<uint32_t>(data, game::kGroupDataAreaId) / 10 - 100);")]),
    ("a part the client always draws counts as a room",
     [("    if (where->outdoors || (where->flags & game::kGroupFlagAlwaysOutdoor) != 0) {",
       "    if (where->outdoors) {")]),
    ("the terrain is not asked about",
     [("    game::IntersectTerrain(point, below, &fraction);\n", "")]),
    ("the lookup goes on below the terrain",
     [("    if (!game::FindGroupOnLine(point, below, fraction, buildings, groups) || buildings[0] == nullptr) {",
       "    if (!game::FindGroupOnLine(point, below, fraction * 0.0f + 1.0f, buildings, groups) || buildings[0] == nullptr) {")]),
    ("the lookup is made with buildings switched off",
     [("    if ((*reinterpret_cast<const uint32_t*>(game::kWorldSwitches) & game::kWorldSwitchBuildings) == 0) {\n"
       "        return false;\n    }\n    const Vec3 below",
       "    const Vec3 below")]),
    ("a group counts as placed whatever the building says",
     [("    if ((At<uint32_t>(building, game::kBuildingFlags) & game::kBuildingFlagGroupsPlaced) == 0) {\n"
       "        return nullptr;\n    }\n", "")]),
    ("a model that has not arrived is used",
     [("    return model != nullptr && At<uint32_t>(model, game::kModelLoaded) != 0 ? model : nullptr;",
       "    return model != nullptr ? model : nullptr;")]),
    ("rooms still loading are taken for ready",
     [("    if (data == nullptr || (At<uint32_t>(data, game::kGroupDataLoadFlags) & game::kGroupDataLoaded) == 0) {",
       "    if (data == nullptr) {")]),
    ("the camera is left in a room the character is not in",
     [("        g_originalLocateCamera();\n        LeaveRoomWithoutCharacter();\n",
       "        g_originalLocateCamera();\n        if (g_state.givenUp) {\n            LeaveRoomWithoutCharacter();\n        }\n")]),
    ("the camera is taken out of a courtyard too",
     [("    if (data == nullptr || GroupIsOutdoors(building, model, data, &area)) {\n        return;",
       "    if (data == nullptr || (GroupIsOutdoors(building, model, data, &area) && false)) {\n        return;")]),
    ("the camera's building stays in the scene's record",
     [("    *reinterpret_cast<void**>(game::kSceneCameraBuilding) = nullptr;\n", "")]),
    ("the camera's second building stays in the scene's record",
     [("    *reinterpret_cast<void**>(game::kSceneCameraBuildingSecond) = nullptr;\n", "")]),
    ("the count of the camera's groups stays in the scene's record",
     [("    *reinterpret_cast<uint32_t*>(game::kSceneCameraGroupCount) = 0;\n", "")]),
    ("the count of the second building's groups stays in the scene's record",
     [("    *reinterpret_cast<uint32_t*>(game::kSceneCameraGroupCountSecond) = 0;\n", "")]),
    ("the camera's building keeps its name in the scene's record",
     [("    *reinterpret_cast<char*>(game::kSceneCameraBuildingName) = '\\0';\n", "")]),
    ("the camera's group keeps its name in the scene's record",
     [("    *reinterpret_cast<char*>(game::kSceneCameraGroupName) = '\\0';\n", "")]),
    ("the client's indoors test of the character overrules what the room is",
     [("    g_state.interior = inRoom;\n", "    g_state.interior = inRoom && !game::UnitIsOutdoors(player);\n")]),

    # The rooms
    ("the client's own walk is made as well",
     [("    ShowFromInside(building, groups);\n    *reinterpret_cast<game::OutdoorWindow*>",
       "    g_originalWalkRooms(building, groups);\n    ShowFromInside(building, groups);\n"
       "    *reinterpret_cast<game::OutdoorWindow*>")]),
    ("the rooms are left to the client's own walk",
     [("    ShowFromInside(building, groups);\n    *reinterpret_cast<game::OutdoorWindow*>",
       "    g_originalWalkRooms(building, groups);\n    if (g_state.givenUp) {\n        ShowFromInside(building, groups);\n    }\n"
       "    *reinterpret_cast<game::OutdoorWindow*>")]),
    ("the window onto the sky is left open",
     [("    *reinterpret_cast<game::OutdoorWindow*>(game::kSceneSkyWindow) = game::kNoOutdoorWindow;\n", "")]),
    ("a courtyard beyond a doorway is drawn, and gone through",
     [("        if ((flags & game::kGroupFlagAlwaysOutdoor) != 0 || GroupIsOutdoors(building, model, data, &area)) {",
       "        if ((flags & game::kGroupFlagAlwaysOutdoor) != 0 && GroupIsOutdoors(building, model, data, &area)) {")]),
    ("a part the client always draws is drawn as a room beyond a doorway",
     [("        if ((flags & game::kGroupFlagAlwaysOutdoor) != 0 || GroupIsOutdoors(building, model, data, &area)) {",
       "        if (GroupIsOutdoors(building, model, data, &area)) {")]),
    ("places beyond doorways that are no rooms are not counted",
     [("            ++g_stats.doorsOut;\n            continue;", "            continue;")]),
    ("the doorways of a room are not gone through",
     [("            reach(doorways[first + i].group);\n", "            (void)first;\n")]),
    ("a room's doorways are looked for at the start of the model's list",
     [("            reach(doorways[first + i].group);", "            reach(doorways[first * 0 + i].group);")]),
    ("a room's count of doorways is not looked at",
     [("        for (uint32_t i = 0; doorways != nullptr && i < count && i < kMaxDoorways; ++i) {",
       "        for (uint32_t i = 0; doorways != nullptr && i < count + 1 && i < kMaxDoorways; ++i) {")]),
    ("only the first of the groups the character is in is started from",
     [("    for (uint32_t i = 0; start != nullptr && i < startCount && i < 4; ++i) {",
       "    for (uint32_t i = 0; start != nullptr && i < startCount && i < 1; ++i) {")]),
    ("rooms above the cut are drawn in the inside view",
     [("        if ((g_state.cutting && box.low.z >= g_state.planeZ) || game::BoxOutsideView(&box)) {",
       "        if (game::BoxOutsideView(&box)) {")]),
    ("rooms off screen are visited in the inside view",
     [("        if ((g_state.cutting && box.low.z >= g_state.planeZ) || game::BoxOutsideView(&box)) {",
       "        if (g_state.cutting && box.low.z >= g_state.planeZ) {")]),
    ("with nothing cut, rooms above where the cut last was are left out",
     [("        if ((g_state.cutting && box.low.z >= g_state.planeZ) || game::BoxOutsideView(&box)) {",
       "        if (box.low.z >= g_state.planeZ || game::BoxOutsideView(&box)) {")]),
    ("a room lit by the sky is marked like any other",
     [("(flags & game::kGroupFlagOutdoorLit) != 0 ? 0 : 1;\n        game::VisitBuildingGroup(index, building);\n"
       "        g_drawn.groups",
       "(flags & game::kGroupFlagOutdoorLit) != 0 ? 1 : 1;\n        game::VisitBuildingGroup(index, building);\n"
       "        g_drawn.groups")]),
    ("the client's marker is not put back after the inside view's rooms",
     [("    *reinterpret_cast<uint32_t*>(game::kSceneInteriorPath) = interiorPath;\n    g_drawn.building = building;",
       "    *reinterpret_cast<uint32_t*>(game::kSceneInteriorPath) = interiorPath * 0 + 1;\n    g_drawn.building = building;")]),
    ("the rooms drawn are not kept for the cursor",
     [("        g_drawn.groups[index / 32] |= 1u << (index % 32);\n", "")]),
    ("the rooms of the cut alone are queued in the inside view too",
     [("    if (g_state.active && !g_state.interior) {", "    if (g_state.active) {")]),
    ("with the view narrowed to a doorway the rooms of the cut alone are queued all the same",
     [("    if (*reinterpret_cast<const int32_t*>(game::kSceneFrustumLevel) != 0) {\n"
       "        return;  // not the whole-screen view this is meant to run under\n    }\n", "")]),
    ("rooms above the cut are queued",
     [("        if (box.low.z >= g_state.planeZ || game::BoxOutsideView(&box)) {",
       "        if (game::BoxOutsideView(&box)) {")]),
    ("outdoor groups are queued as rooms",
     [("        if ((flags & (game::kGroupFlagOutdoor | game::kGroupFlagAlwaysOutdoor)) != 0) {\n"
       "            continue;  // the outdoor pass has these\n        }\n", "")]),
    ("the line of sight is asked about in the inside view",
     [("const bool sight = !inRoom && (g_state.indoors || g_settings.outdoors) && LowestBetween(chest, eye, &sightZ);",
       "const bool sight = (g_state.indoors || g_settings.outdoors) && LowestBetween(chest, eye, &sightZ);")]),
    ("a block from outdoors goes on counting in a hall seen from inside",
     [("    } else if (inRoom) {", "    } else if (inRoom && g_state.givenUp) {")]),
    ("the inside view stops when Direct3D refuses the cut",
     [("        // Direct3D does not take the cut. The inside view goes on without.\n",
       "        g_state.interior = false;\n")]),

    # The background
    ("the screen is cleared to the client's colour",
     [("    g_originalSceneClear(what, kBlack);\n", "    g_originalSceneClear(what, colour | (kBlack & 0));\n")]),
    ("the screen is always cleared to black",
     [("    if (!g_state.interior) {\n        g_originalSceneClear(what, colour);\n        return;\n    }",
       "    if (!g_state.interior) {\n        g_originalSceneClear(what, (colour & 0) | kBlack);\n        return;\n    }")]),
    ("the background is left at the far end of the view",
     [("        game::Direct3DClearDepth(direct3d, depth);\n", "        (void)direct3d;\n")]),
    ("the background stands right at the farthest corner",
     [("(g_drawn.reach + kBehindRooms);", "(g_drawn.reach + kBehindRooms * 0.0f);")]),
    ("the background stands behind the last room drawn, not the farthest",
     [("        g_drawn.reach = Max(g_drawn.reach, FarthestCorner(box, g_state.eye));",
       "        g_drawn.reach = FarthestCorner(box, g_state.eye);")]),
    ("the nearest corner of a room is taken for its farthest",
     [("    const float x = Max(Abs(box.low.x - from.x), Abs(box.high.x - from.x));",
       "    const float x = Min(Abs(box.low.x - from.x), Abs(box.high.x - from.x));")]),
    ("the background's depth leaves out the far distance's share",
     [("    const float depth = hardware.m[2][2] + hardware.m[3][2] / (g_drawn.reach + kBehindRooms);",
       "    const float depth = 1.0f + hardware.m[3][2] / (g_drawn.reach + kBehindRooms) * 0.9f;")]),
    ("depth is cleared although no room was drawn",
     [("    if (device == nullptr || !(g_drawn.reach > 0.0f)) {", "    if (device == nullptr) {")]),

    # The cut
    ("the plane is measured from the wrong height",
     [("const float plane[4] = {-view.m[2][0], -view.m[2][1], -view.m[2][2], g_state.planeZ - eye.z};",
       "const float plane[4] = {-view.m[2][0], -view.m[2][1], -view.m[2][2], g_state.planeZ - eye.z + 1.0f};")]),
    ("the plane is handed over as it is in view space",
     [("    const float clip[4] = {plane[0] / hardware.m[0][0], plane[1] / hardware.m[1][1], plane[3] / hardware.m[3][2],\n"
       "                           plane[2] - hardware.m[2][2] * plane[3] / hardware.m[3][2]};",
       "    const float clip[4] = {plane[0] + 0.0f * hardware.m[0][0], plane[1], plane[2], plane[3]};")]),
    ("the plane's sideways term is not divided",
     [("    const float clip[4] = {plane[0] / hardware.m[0][0],", "    const float clip[4] = {plane[0] + 0.0f * hardware.m[0][0],")]),
    ("the plane's upward term is not divided",
     [("plane[1] / hardware.m[1][1], plane[3] / hardware.m[3][2],", "plane[1] + 0.0f * hardware.m[1][1], plane[3] / hardware.m[3][2],")]),
    ("the plane's last term leaves out the far distance's share",
     [("                           plane[2] - hardware.m[2][2] * plane[3] / hardware.m[3][2]};",
       "                           plane[2] - 0.9f * hardware.m[2][2] * plane[3] / hardware.m[3][2]};")]),
    ("a projection of another shape is cut all the same",
     [("    if (!IsWorldProjection(projection) || !IsWorldProjection(hardware)) {",
       "    if ((!IsWorldProjection(projection) || !IsWorldProjection(hardware)) && g_state.givenUp) {")]),
    ("where the matrices put the character is not checked",
     [("          Abs(feetView[1] * projection.m[1][1]) <= depth)) {",
       "          Abs(feetView[1] * projection.m[1][1]) <= depth) && g_state.givenUp) {")]),
    ("a device that is not Direct3D's is used all the same",
     [("    if (device == nullptr || !game::IsDirect3DDevice(device)) {", "    if (device == nullptr) {")]),
    ("the cut uses the clip plane the client uses itself",
     [("constexpr uint32_t kClipPlane = 5;", "constexpr uint32_t kClipPlane = 0;")]),
    ("a plane Direct3D refused is switched on all the same",
     [("    if (game::Direct3DSetClipPlane(direct3d, kClipPlane, clip) < 0) {",
       "    if (game::Direct3DSetClipPlane(direct3d, kClipPlane, clip) < 0 && g_state.givenUp) {")]),
    ("the cutaway never gives up on a Direct3D that refuses",
     [("        if (++g_state.refusals >= kMaxRefusals) {", "        if (++g_state.refusals >= kMaxRefusals + 1000) {")]),
    ("draws without a vertex shader are cut",
     [("        cut = states != nullptr && At<void*>(states, game::kGxStateVertexShader * game::kGxStateSize) != nullptr &&\n",
       "        cut = states != nullptr &&\n")]),
    ("draws with another projection are cut",
     [("              SameMatrix(At<Matrix>(device, game::kGxDeviceHardwareProjection), g_cut.projection);",
       "              (SameMatrix(At<Matrix>(device, game::kGxDeviceHardwareProjection), g_cut.projection) || true);")]),
    ("the client's own clip plane is switched off by the cut",
     [("ClientsClipPlanes(device) | kClipPlaneBit);", "kClipPlaneBit);")]),
    ("the switch is sent only when the library thinks it has changed",
     [("    if (cut) {\n        game::Direct3DSetRenderState(direct3d, game::kDirect3DClipPlaneEnable, ClientsClipPlanes(device) | kClipPlaneBit);\n",
       "    if (cut) {\n        if (!g_cut.sent) {\n            game::Direct3DSetRenderState(direct3d, game::kDirect3DClipPlaneEnable, ClientsClipPlanes(device) | kClipPlaneBit);\n        }\n")]),
    ("a draw that is not cut loses the client's own clip plane",
     [("    } else if (g_cut.sent) {\n        game::Direct3DSetRenderState(direct3d, game::kDirect3DClipPlaneEnable, ClientsClipPlanes(device));",
       "    } else if (g_cut.sent) {\n        game::Direct3DSetRenderState(direct3d, game::kDirect3DClipPlaneEnable, ClientsClipPlanes(device) * 0);")]),
    ("a draw that is not cut keeps the plane on",
     [("    } else if (g_cut.sent) {\n        game::Direct3DSetRenderState(direct3d, game::kDirect3DClipPlaneEnable, ClientsClipPlanes(device));\n"
       "        g_cut.sent = false;",
       "    } else if (g_cut.sent && g_state.givenUp) {\n        game::Direct3DSetRenderState(direct3d, game::kDirect3DClipPlaneEnable, ClientsClipPlanes(device));\n"
       "        g_cut.sent = false;")]),
    ("the plane stays on after the world has been drawn",
     [("    g_originalWorldDrawn(list, from);\n    StopCut();\n", "    g_originalWorldDrawn(list, from);\n")]),
    ("the plane is switched off before the last of the world is drawn",
     [("    g_originalWorldDrawn(list, from);\n    StopCut();\n", "    StopCut();\n    g_originalWorldDrawn(list, from);\n")]),
    ("the end of the cut takes the client's own clip plane with it",
     [("        game::Direct3DSetRenderState(direct3d, game::kDirect3DClipPlaneEnable, ClientsClipPlanes(device));\n    }\n}",
       "        game::Direct3DSetRenderState(direct3d, game::kDirect3DClipPlaneEnable, ClientsClipPlanes(device) * 0);\n    }\n}")]),
    ("a device that has sent nothing yet is taken to have every plane on",
     [("    return sent == game::kGxNothingSent ? 0 : sent;", "    return sent;")]),
    ("a frame that never came to its end leaves the plane on",
     [("    StopCut();  // had last frame's drawing not come to its usual end\n", "")]),
    ("a UI reload leaves the plane on",
     [("void Reset() {\n    StopCut();\n", "void Reset() {\n")]),
    ("a draw without a scene update before it is cut",
     [("        g_state.active = false;  // one scene update prepares one draw\n", "")]),
    ("the draws made with the cut are not counted",
     [("        g_cut.sent = true;\n        ++g_stats.draws;", "        g_cut.sent = true;")]),

    # Outdoors: what is in the way
    ("under the open sky the cut is for everything, as it used to be",
     [("    return !g_canChoose || g_state.interior || !g_state.open || g_state.unnamed;",
       "    return !g_canChoose || g_state.interior || !g_state.open || g_state.unnamed || true;")]),
    ("with the view blocked and no part named, nothing is cut",
     [("            } else if (g_state.sightNow) {\n                g_state.unnamed = true;",
       "            } else if (g_state.sightNow) {\n                g_state.unnamed = false;")]),
    ("a cut for everything turns into a cut for nothing the moment the view clears",
     [("            } else if (g_state.sightNow) {\n                g_state.unnamed = true;\n            }",
       "            } else if (g_state.sightNow) {\n                g_state.unnamed = true;\n            } else {\n"
       "                g_state.unnamed = false;\n            }")]),
    ("once everything was cut, naming a part does not bring the rest back",
     [("            if (g_way.count != 0) {\n                g_state.unnamed = false;",
       "            if (g_way.count != 0) {\n                g_state.unnamed = g_state.unnamed && true;")]),
    ("a line of sight is tried from the character's end only",
     [("(GroupOnLine(building, model, index, data, target, eye) ||\n"
       "                                                         GroupOnLine(building, model, index, data, eye, target))",
       "(GroupOnLine(building, model, index, data, target, eye))")]),
    ("only the line to the chest is tried",
     [("        for (const Vec3& target : targets) {\n            // Both ways",
       "        for (const Vec3& each : targets) {\n            const Vec3& target = *(&each - (&each - targets));\n"
       "            // Both ways")]),
    ("the line to the head is not tried",
     [("        {feet.x, feet.y, feet.z + kHead},\n", "        {feet.x, feet.y, feet.z + kHead * 0.0f + kChest},\n")]),
    ("the lines to either side are not tried",
     [("        {feet.x + sideX, feet.y + sideY, feet.z + kChest},\n        {feet.x - sideX, feet.y - sideY, feet.z + kChest},\n",
       "        {feet.x + sideX * 0.0f, feet.y + sideY * 0.0f, feet.z + kChest},\n"
       "        {feet.x - sideX * 0.0f, feet.y - sideY * 0.0f, feet.z + kChest},\n")]),
    ("the lines to either side run along the line of sight, not across it",
     [("    float sideX = feet.y - eye.y;\n    float sideY = eye.x - feet.x;",
       "    float sideX = eye.x - feet.x;\n    float sideY = eye.y - feet.y;")]),
    ("the lines to either side are two yards out",
     [("constexpr float kBeside = 1.0f;", "constexpr float kBeside = 2.0f;")]),
    ("a part is forgotten the moment it is out of the way",
     [("        if (now - g_way.parts[i].seen <= kInTheWayMs) {", "        if (now - g_way.parts[i].seen <= kInTheWayMs * 0) {")]),
    ("a part is never forgotten",
     [("        if (now - g_way.parts[i].seen <= kInTheWayMs) {",
       "        if (now - g_way.parts[i].seen <= kInTheWayMs + 1000000) {")]),
    ("the list of groups is walked whatever its layout",
     [("        return;  // the list is not laid out as read\n", "        (void)0;\n")]),
    ("a part is known by its group's number alone",
     [("        if (g_way.parts[i].building == building && g_way.parts[i].group == group) {\n            return true;",
       "        if ((g_way.parts[i].building == building || true) && g_way.parts[i].group == group) {\n"
       "            return true;")]),
    ("a part is remembered under whatever group was found first",
     [("        if (g_way.parts[i].building == building && g_way.parts[i].group == group) {\n            slot = i;",
       "        if (g_way.parts[i].building == building) {\n            slot = i;")]),
    ("a room goes with a part of any building",
     [("        if (g_way.parts[i].building == building && BoxesTouch(g_way.parts[i].box, box)) {",
       "        if ((g_way.parts[i].building == building || true) && BoxesTouch(g_way.parts[i].box, box)) {")]),
    ("a room goes with a part however far off it is",
     [("        if (g_way.parts[i].building == building && BoxesTouch(g_way.parts[i].box, box)) {",
       "        if (g_way.parts[i].building == building && (BoxesTouch(g_way.parts[i].box, box) || true)) {")]),
    ("boxes one above the other count as touching",
     [("           a.low.z <= b.high.z && b.low.z <= a.high.z;", "           true;")]),
    ("the courtyard next to the house is cut with it",
     [("    if (GroupIsOutdoors(building, model, data, &area)) {\n        return false;\n    }\n    const uint8_t* placed",
       "    if (GroupIsOutdoors(building, model, data, &area) && false) {\n        return false;\n    }\n"
       "    const uint8_t* placed")]),
    ("the part that is always drawn goes with the house",
     [("    if (data == nullptr || (At<uint32_t>(data, game::kGroupDataFlags) & game::kGroupFlagAlwaysOutdoor) != 0) {\n"
       "        return false;\n    }\n    int32_t area = -1;",
       "    if (data == nullptr) {\n        return false;\n    }\n    int32_t area = -1;")]),
    ("no room goes with the part in the way",
     [("    if (!HasPartInTheWay(building) || ReadyModel(building) != model) {",
       "    if (HasPartInTheWay(building) || ReadyModel(building) != model) {")]),
    ("a group in the way is looked up under the wrong building",
     [("                  GroupIsCut(model, group, placement - game::kBuildingPlacement);",
       "                  GroupIsCut(model, group, placement);")]),
    ("the plane stays on behind a group that was cut",
     [("    g_originalDrawGroup(model, group, placement, views);\n    g_cut.piece = false;",
       "    g_originalDrawGroup(model, group, placement, views);")]),
    ("groups are chosen even while everything is cut",
     [("    g_cut.piece = g_cut.on && !g_cut.all && model != nullptr && placement != nullptr &&\n"
       "                  GroupIsCut(model, group, placement - game::kBuildingPlacement);\n"
       "    g_originalDrawGroup(model, group, placement, views);",
       "    g_cut.piece = g_cut.on && !g_cut.all && model != nullptr && placement != nullptr &&\n"
       "                  GroupIsCut(model, group, placement - game::kBuildingPlacement);\n"
       "    g_cut.all = false;\n"
       "    g_originalDrawGroup(model, group, placement, views);")]),
    ("the plane is on for every draw whatever is being drawn",
     [("    bool cut = g_cut.on && (g_cut.all || g_cut.piece);", "    bool cut = g_cut.on;")]),
    ("the plane is on only for the pieces chosen, indoors too",
     [("    bool cut = g_cut.on && (g_cut.all || g_cut.piece);", "    bool cut = g_cut.on && g_cut.piece;")]),
    ("what is high enough is left out wherever it stands",
     [("           StandsByPartInTheWay(centre);\n}", "           (StandsByPartInTheWay(centre) || true);\n}")]),
    ("nothing is left out with a part in the way",
     [("           StandsByPartInTheWay(centre);\n}", "           StandsByPartInTheWay(centre) && false;\n}")]),
    ("what reaches below the cut is left out with the rest",
     [("    return g_state.drawnCut && g_way.count != 0 && centre.z - radius > g_state.planeZ &&",
       "    return g_state.drawnCut && g_way.count != 0 && radius > 0.0f &&")]),
    ("a piece is judged by its middle, not by its foot",
     [("    return g_state.drawnCut && g_way.count != 0 && centre.z - radius > g_state.planeZ &&",
       "    return g_state.drawnCut && g_way.count != 0 && centre.z - radius * 0.0f > g_state.planeZ &&")]),
    ("pieces are left out although the cut is not being made",
     [("    return g_state.drawnCut && g_way.count != 0 && centre.z - radius > g_state.planeZ &&",
       "    return g_way.count != 0 && centre.z - radius > g_state.planeZ &&")]),
    ("a frame drawn without its cut is taken for one drawn with it",
     [("    ++g_stats.framesSkipped;\n    g_state.drawnCut = false;\n", "    ++g_stats.framesSkipped;\n")]),
    ("the parts in the way outlive the cut they belong to",
     [("    if (!g_state.active || !g_state.open) {\n        g_way.count = 0;",
       "    if (!g_state.active && !g_installed) {\n        g_way.count = 0;")]),
    ("what stands high over a part in the way is not counted to it",
     [("            point.y <= box.high.y + kAround && point.z >= box.low.z - kAround) {",
       "            point.y <= box.high.y + kAround && point.z >= box.low.z - kAround && point.z <= box.high.z + kAround) {")]),
    ("what stands under a part in the way goes with it too",
     [("            point.y <= box.high.y + kAround && point.z >= box.low.z - kAround) {",
       "            point.y <= box.high.y + kAround) {")]),
    ("scenery counts as by a part from much further off",
     [("constexpr float kAround = 1.5f;", "constexpr float kAround = 30.0f;")]),
    ("what sticks out of a part's box a little does not go with it",
     [("constexpr float kAround = 1.5f;", "constexpr float kAround = 0.0f;")]),
    ("the group's own test gets one end of the line in the world's coordinates",
     [("    const Vec3 line[2] = {ToModel(from, placement), ToModel(to, placement)};",
       "    const Vec3 line[2] = {ToModel(from, placement), to};")]),
    ("the model's turn is applied the wrong way round",
     [("    return {point.x * m[0][0] + point.y * m[1][0] + point.z * m[2][0] + m[3][0],",
       "    return {point.x * m[0][0] + point.y * m[0][1] + point.z * m[0][2] + m[3][0],")]),
    ("the model's place in the world is left out",
     [("            point.x * m[0][1] + point.y * m[1][1] + point.z * m[2][1] + m[3][1],",
       "            point.x * m[0][1] + point.y * m[1][1] + point.z * m[2][1],")]),
    ("a group's box is not asked about before its surfaces",
     [("    if (!game::LineTouchesGroupBox(model, &line[0], &line[1], index)) {\n        return false;\n    }\n",
       "    (void)model;\n    (void)index;\n")]),
    ("the line of sight is judged by every kind of surface",
     [("game::kIntersectBuildings, game::kBuildingSurfacesPassed, &out,", "game::kIntersectBuildings, 0, &out,")]),
    ("the group's own test is not told what kind of line it is",
     [("game::kIntersectBuildings, game::kBuildingSurfacesPassed, &out,", "0, game::kBuildingSurfacesPassed, &out,")]),
    ("under the open sky every room in view is queued, whatever is cut",
     [("        if (!everything && !TouchesPartInTheWay(building, box)) {",
       "        if (!everything && !TouchesPartInTheWay(building, box) && false) {")]),

    # Scenery and creatures
    ("scenery outside the rooms shown is drawn",
     [("        keep = scenery ? SceneryInShownRoom(piece, *centre) : CreatureInShownRoom(*centre);",
       "        keep = scenery ? (SceneryInShownRoom(piece, *centre) || true) : CreatureInShownRoom(*centre);")]),
    ("creatures outside the rooms shown are drawn",
     [("        keep = scenery ? SceneryInShownRoom(piece, *centre) : CreatureInShownRoom(*centre);",
       "        keep = scenery ? SceneryInShownRoom(piece, *centre) : (CreatureInShownRoom(*centre) || true);")]),
    ("scenery and creatures are left out outside the inside view too",
     [("    } else if (g_state.interior) {\n        keep = scenery ?", "    } else if (g_frame != 0) {\n        keep = scenery ?")]),
    ("outdoors nothing is ever left out",
     [("        keep = !GoesWithTheCut(*centre, radius);", "        keep = !GoesWithTheCut(*centre, radius) || radius >= 0.0f;")]),
    ("a piece of scenery left out still counts as in view for the client",
     [("    return seen != 0 && !KeepPiece(centre, radius, true) ? 0 : seen;", "    return seen != 0 && !KeepPiece(centre, radius, true) ? seen : seen;")]),
    ("a creature left out still counts as in view for the client",
     [("    return seen != 0 && !KeepPiece(centre, radius, false) ? 0 : seen;", "    return seen != 0 && !KeepPiece(centre, radius, false) ? seen : seen;")]),
    ("the client's answer about scenery is replaced by a plain yes",
     [("    return seen != 0 && !KeepPiece(centre, radius, true) ? 0 : seen;", "    return seen != 0 && !KeepPiece(centre, radius, true) ? 0 : (seen != 0 ? 1 : 0);")]),
    ("scenery the client says is out of view is asked about and shown",
     [("    return seen != 0 && !KeepPiece(centre, radius, true) ? 0 : seen;", "    return KeepPiece(centre, radius, true) ? (seen | 1) : 0;")]),
    ("a creature the client says is out of view is asked about anyway",
     [("    return seen != 0 && !KeepPiece(centre, radius, false) ? 0 : seen;", "    return !KeepPiece(centre, radius, false) ? 0 : seen;")]),
    ("creatures are treated as scenery",
     [("    return seen != 0 && !KeepPiece(centre, radius, false) ? 0 : seen;", "    return seen != 0 && !KeepPiece(centre, radius, true) ? 0 : seen;")]),
    ("a room that is not shown counts as one that is",
     [("    return kept.building != nullptr && IsDrawnRoom(kept.building, kept.group);", "    return kept.building != nullptr;")]),
    ("a creature in a room that is not shown is drawn",
     [("    return StandsIn(point, &room) && IsDrawnRoom(room.building, room.group);",
       "    return StandsIn(point, &room);")]),
    ("scenery is asked about every frame",
     [("    if (!same || (kept.building == nullptr && g_frame - kept.frame >= kRecheckFrames)) {", "    if (!same || kRecheckFrames != 0) {")]),
    ("a piece of scenery is taken for whatever had its address before",
     [("    const bool same = kept.piece == piece && kept.centre.x == centre.x && kept.centre.y == centre.y &&\n"
       "                      kept.centre.z == centre.z;",
       "    const bool same = kept.piece == piece;")]),
    ("scenery found in no room is never asked about again",
     [("g_frame - kept.frame >= kRecheckFrames)) {", "g_frame - kept.frame >= kRecheckFrames + 1000000)) {")]),
    ("scenery found in a room is asked about again all the same",
     [("    if (!same || (kept.building == nullptr && g_frame - kept.frame >= kRecheckFrames)) {",
       "    if (!same || g_frame - kept.frame >= kRecheckFrames) {")]),
    ("the character's company is asked about like anyone",
     [("    if (!scenery && WithinReach(*centre)) {", "    if (!scenery && WithinReach(*centre) && g_state.givenUp) {")]),
    ("scenery near the character is drawn wherever it stands",
     [("    if (!scenery && WithinReach(*centre)) {", "    if (WithinReach(*centre)) {")]),
    ("the character's reach is counted along the ground alone",
     [("    return aside <= kOwnReach * kOwnReach && Abs(centre.z - feet.z) <= kOwnReach;",
       "    return aside <= kOwnReach * kOwnReach;")]),
    ("the character's reach is a good deal longer",
     [("constexpr float kOwnReach = 8.0f;", "constexpr float kOwnReach = 12.0f;")]),
    ("the character's reach is a good deal shorter",
     [("constexpr float kOwnReach = 8.0f;", "constexpr float kOwnReach = 6.0f;")]),
    ("a piece left out is counted every time it is asked about",
     [("    if (!keep && AddThisFrame(g_leftOut, kLeftOutSlots, piece)) {",
       "    if (!keep && (AddThisFrame(g_leftOut, kLeftOutSlots, piece) || true)) {")]),
    ("a piece left out once is never counted again",
     [("        Slot& slot = slots[(first + i) & (count - 1)];\n        if (slot.frame != frame) {",
       "        Slot& slot = slots[(first + i) & (count - 1)];\n        if (slot.pointer == nullptr) {")]),
    ("creatures left out are counted as scenery",
     [("        ++(scenery ? g_stats.sceneryOutNow : g_stats.creaturesOutNow);", "        ++g_stats.sceneryOutNow;")]),
    ("what was left out is reported a frame late",
     [("    g_originalFinishVisits();\n    g_stats.sceneryOut = g_stats.sceneryOutNow;\n    g_stats.creaturesOut = g_stats.creaturesOutNow;\n",
       "    g_stats.sceneryOut = g_stats.sceneryOutNow;\n    g_stats.creaturesOut = g_stats.creaturesOutNow;\n    g_originalFinishVisits();\n")]),
    ("the counts of what was left out are never started afresh",
     [("    g_stats.sceneryOutNow = 0;\n    g_stats.creaturesOutNow = 0;\n", "")]),
    ("the look under a piece does not reach the floor",
     [("constexpr float kStandingProbe = 300.0f;", "constexpr float kStandingProbe = 0.4f; ")]),
    ("the look under a piece goes no deeper than a house is high",
     [("constexpr float kStandingProbe = 300.0f;", "constexpr float kStandingProbe = 40.0f; ")]),
    ("the look under a piece begins at its very centre",
     [("    const Vec3 from = {centre.x, centre.y, centre.z + kStandingLift};",
       "    const Vec3 from = {centre.x, centre.y, centre.z + kStandingLift * 0.0f};")]),
    ("the look under a piece begins well over it",
     [("constexpr float kStandingLift = 0.5f; ", "constexpr float kStandingLift = 12.0f;")]),
    ("nobody looks overhead where nothing is under a piece",
     [("    if (where.found) {\n        return false;\n    }\n    const Vec3 above",
       "    if (where.found || kStandingProbe > 0.0f) {\n        return false;\n    }\n    const Vec3 above")]),
    ("what is overhead is asked even where a street is underfoot",
     [("    if (where.found) {\n        return false;\n    }\n    const Vec3 above", "    const Vec3 above")]),
    ("the look overhead goes down instead",
     [("    const Vec3 above = {from.x, from.y, from.z + kStandingProbe};",
       "    const Vec3 above = {from.x, from.y, from.z - kStandingProbe};")]),

    # The cursor
    ("the cursor ray is left alone",
     [("start->z = g_state.planeZ;\n    }", "}")]),
    ("a point counts as in a drawn room by what is below it alone",
     [("    return RoomOnLine(point, below, &building, &group) && IsDrawnRoom(building, group) &&\n"
       "           RoomOnLine(point, above, &building, &group) && IsDrawnRoom(building, group);",
       "    return RoomOnLine(point, below, &building, &group) && IsDrawnRoom(building, group) && above.z > below.z;")]),
    ("any room of the building counts as drawn",
     [("    return building != nullptr && building == g_drawn.building && group < static_cast<uint32_t>(kMaxGroups) &&\n"
       "           (g_drawn.groups[group / 32] >> (group % 32) & 1u) != 0;",
       "    return building != nullptr && building == g_drawn.building && group < static_cast<uint32_t>(kMaxGroups);")]),
    ("a room of any building counts as drawn",
     [("    return building != nullptr && building == g_drawn.building && group < static_cast<uint32_t>(kMaxGroups) &&",
       "    return building != nullptr && group < static_cast<uint32_t>(kMaxGroups) &&")]),
    ("the ray is picked up on the surface, not past it",
     [("        const float along = fraction + kPast / span;", "        const float along = fraction + kPast * 0.0f / span;")]),
    ("the ray is taken past one surface more",
     [("    for (int unseen = 0; unseen <= kMaxUnseen; ++unseen) {", "    for (int unseen = 0; unseen <= kMaxUnseen + 1; ++unseen) {")]),
    ("the ray does not enter the drawn room at all",
     [("    if (g_state.interior) {\n        EnterDrawnRoom(start, *end);", "    if (g_state.interior && g_state.givenUp) {\n        EnterDrawnRoom(start, *end);")]),
    ("the ray looks for rooms even with buildings switched off",
     [("    if (g_drawn.building == nullptr ||\n        (*reinterpret_cast<const uint32_t*>(game::kWorldSwitches) & game::kWorldSwitchBuildings) == 0) {",
       "    if (g_drawn.building == nullptr) {")]),
    ("the unseen surfaces are looked for among buildings only",
     [("game::IntersectWorld(point, end, &hit, &fraction, game::kIntersectClick)",
       "game::IntersectWorld(point, end, &hit, &fraction, game::kIntersectBuildings)")]),
]

def run_one(mutation, scratch, source):
    name, changes = mutation
    missing = [old for old, _ in changes if source.count(old) != 1]
    if missing:
        return name, "SETUP", "this text is not in cutaway.cpp exactly once: %r" % missing[0][:70]
    mutated = source
    for old, new in changes:
        mutated = mutated.replace(old, new)
    with open(os.path.join(scratch, FILE), "w") as file:
        file.write(mutated)
    run = subprocess.run(["bash", "test/run.sh"], cwd=scratch, capture_output=True, text=True)
    output = run.stdout + run.stderr
    fails = [line.strip()[4:].strip() for line in output.splitlines() if line.strip().startswith("FAIL")]
    if run.returncode != 0 and fails:
        return name, "caught", "%d checks, first: %s" % (len(fails), fails[0][:70])
    if run.returncode != 0:
        errors = [line for line in output.splitlines() if "error" in line.lower()]
        return name, "BUILD", errors[0].strip() if errors else "the run failed without a failed check"
    return name, "MISSED", "every check passed"


def main():
    arguments = sys.argv[1:]
    jobs = 4
    if "--jobs" in arguments:
        at = arguments.index("--jobs")
        jobs = int(arguments[at + 1])
        del arguments[at:at + 2]
    chosen = [m for m in MUTATIONS if not arguments or any(word in m[0] for word in arguments)]
    with open(os.path.join(CLIENT, FILE)) as file:
        source = file.read()

    with tempfile.TemporaryDirectory(prefix="wotlk-arpg-mutation-") as root:
        scratch = []
        for job in range(min(jobs, max(len(chosen), 1))):
            path = os.path.join(root, str(job), "client")
            shutil.copytree(CLIENT, path, ignore=shutil.ignore_patterns("build"))
            scratch.append(path)

        def worker(index):
            return [run_one(m, scratch[index], source) for m in chosen[index::len(scratch)]]

        results = {}
        with concurrent.futures.ThreadPoolExecutor(len(scratch)) as pool:
            for batch in pool.map(worker, range(len(scratch))):
                for name, verdict, detail in batch:
                    results[name] = (verdict, detail)

    bad = 0
    for name, _ in chosen:
        verdict, detail = results[name]
        bad += verdict != "caught"
        print("%-7s %s\n        (%s)" % (verdict, name, detail))
    print()
    print("%d breakages, %s" % (len(chosen), "all caught" if not bad else "%d NOT caught" % bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
