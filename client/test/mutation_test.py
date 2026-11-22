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
     [("const Vec3 chest = {feet.x, feet.y, feet.z + kChest};",
       "const Vec3 chest = {feet.x, feet.y, feet.z + kChest - 3.0f};")]),
    ("the line of sight starts at the knees, where a low wall is in the way",
     [("const Vec3 chest = {feet.x, feet.y, feet.z + kChest};",
       "const Vec3 chest = {feet.x, feet.y, feet.z + kChest - 1.0f};")]),
    ("a hit that contradicts itself is believed",
     [("    if (Abs(hit.z - fromStart) > 0.5f && Abs(hit.z - fromEnd) > 0.5f) {",
       "    if (Abs(hit.z - fromStart) > 1000.0f && Abs(hit.z - fromEnd) > 1000.0f) {")]),
    ("the two heights in the status are the wrong way round",
     [("    game::lua_pushnumber(L, static_cast<double>(g_state.overhead));\n"
       "    game::lua_pushnumber(L, static_cast<double>(g_state.between));",
       "    game::lua_pushnumber(L, static_cast<double>(g_state.between));\n"
       "    game::lua_pushnumber(L, static_cast<double>(g_state.overhead));")]),

    # The projection
    ("the sideways depth term is not halved",
     [("cut.m[0][2] = 0.5f * scale * plane[0];", "cut.m[0][2] = scale * plane[0];")]),
    ("the upward depth term is not halved",
     [("cut.m[1][2] = 0.5f * scale * plane[1];", "cut.m[1][2] = scale * plane[1];")]),
    ("the sideways depth term is not halved, and the check on the device agrees",
     [("cut.m[0][2] = 0.5f * scale * plane[0];", "cut.m[0][2] = scale * plane[0];"),
      ("Close(hardware.m[0][2], half * plane[0])", "Close(hardware.m[0][2], scale * plane[0])")]),
    ("the upward depth term is not halved, and the check on the device agrees",
     [("cut.m[1][2] = 0.5f * scale * plane[1];", "cut.m[1][2] = scale * plane[1];"),
      ("Close(hardware.m[1][2], half * plane[1])", "Close(hardware.m[1][2], scale * plane[1])")]),
    ("the plane is measured from the wrong height",
     [("const float plane[4] = {-view.m[2][0], -view.m[2][1], -view.m[2][2], g_state.planeZ - eye.z};",
       "const float plane[4] = {-view.m[2][0], -view.m[2][1], -view.m[2][2], g_state.planeZ - eye.z + 1.0f};")]),
    ("the shaders are not told",
     [("    game::UpdateShaderProjection();\n    ++g_stats.framesCut;", "    ++g_stats.framesCut;")]),
    ("a device that makes something else of the projection is not noticed",
     [("          Close(hardware.m[2][3], 1.0f) && IsZero(hardware.m[3][3]))) {",
       "          Close(hardware.m[2][3], 1.0f) && IsZero(hardware.m[3][3])) && false) {")]),
    ("a projection of another shape is cut all the same",
     [("    if (!IsWorldProjection(projection)) {", "    if (!IsWorldProjection(projection) && false) {")]),

    # Rooms, the camera's building, the cursor
    ("rooms above the cut are queued",
     [("        if (box.low.z >= g_state.planeZ || game::BoxOutsideView(&box)) {",
       "        if (game::BoxOutsideView(&box)) {")]),
    ("rooms still loading are queued",
     [("if (data == nullptr || (At<uint32_t>(data, game::kGroupDataLoadFlags) & game::kGroupDataLoaded) == 0) {",
       "if (data == nullptr) {")]),
    ("outdoor groups are queued as rooms",
     [("        if ((flags & (game::kGroupFlagOutdoor | game::kGroupFlagAlwaysOutdoor)) != 0) {\n"
       "            continue;  // the outdoor pass has these\n        }\n", "")]),
    ("the cursor ray is left alone",
     [("start->z = g_state.planeZ;\n    }", "}")]),

    # The inside view
    ("the room is looked for under the camera, not under the character",
     [("    *camera = {feet.x, feet.y, feet.z + kChest};\n", "")]),
    ("the camera's position is not put back",
     [("    g_originalLocateCamera();\n    *camera = eye;\n", "    g_originalLocateCamera();\n")]),
    ("the client's indoors test overrules the scene's own answer",
     [("    g_state.interior = inRoom;\n", "    g_state.interior = inRoom && !game::UnitIsOutdoors(player);\n")]),
    ("the window onto the outdoor world is left open",
     [("        *reinterpret_cast<game::OutdoorWindow*>(game::kSceneOutdoorWindow) = game::kNoOutdoorWindow;\n", "")]),
    ("the window onto the sky is left open",
     [("        *reinterpret_cast<game::OutdoorWindow*>(game::kSceneSkyWindow) = game::kNoOutdoorWindow;\n", "")]),
    ("the screen is cleared to the client's colour",
     [("g_originalSceneClear(what, g_state.interior ? kBlack : colour);", "g_originalSceneClear(what, colour);")]),
    ("the screen is always cleared to black",
     [("g_originalSceneClear(what, g_state.interior ? kBlack : colour);",
       "g_originalSceneClear(what, g_state.interior || what != 0x7FFF ? kBlack : colour);")]),
    ("the fog keeps its colour",
     [("g_originalSetFog(start + g_state.away, end + g_state.away, density, &black);",
       "g_originalSetFog(start + g_state.away, end + g_state.away, density, black == kBlack ? colour : &black);")]),
    ("the fog still starts at the camera",
     [("g_originalSetFog(start + g_state.away, end + g_state.away, density, &black);",
       "g_originalSetFog(start + g_state.away * 0.0f, end + g_state.away, density, &black);")]),
    ("the fog ends where it used to",
     [("g_originalSetFog(start + g_state.away, end + g_state.away, density, &black);",
       "g_originalSetFog(start + g_state.away, end + g_state.away * 0.0f, density, &black);")]),
    ("the fog is changed in a hall the camera is in",
     [("    if (g_state.fromOutside && g_settings.enabled) {", "    if (g_state.interior && g_settings.enabled) {")]),
    ("rooms the walk did not reach are visited",
     [("        if (placed == nullptr || At<uint32_t>(placed, game::kPlacedGroupQueued) == 0) {",
       "        if (placed == nullptr) {")]),
    ("rooms the walk reached are not given the whole screen",
     [("        if (data != nullptr && (At<uint32_t>(data, game::kGroupDataLoadFlags) & game::kGroupDataLoaded) != 0) {\n"
       "            game::VisitBuildingGroup(index, building);\n        }\n", "        (void)data;\n")]),
    ("the second visits mark the rooms afresh",
     [("    *reinterpret_cast<uint32_t*>(game::kSceneInteriorPath) = 0;\n", 
       "    *reinterpret_cast<uint32_t*>(game::kSceneInteriorPath) = 1;\n")]),
    ("with the view narrowed to a doorway the rooms are visited all the same",
     [("    if (building == nullptr || *reinterpret_cast<const int32_t*>(game::kSceneFrustumLevel) != 0 ||",
       "    if (building == nullptr ||")]),
    ("the rooms of the cut alone are queued in the inside view too",
     [("    if (g_state.interior) {\n        ShowDrawnRooms();\n    } else if (g_state.active) {",
       "    if (g_state.interior) {\n        ShowDrawnRooms();\n    }\n    if (g_state.active) {")]),
    ("the line of sight is asked about in the inside view",
     [("const bool sight = !inRoom && (g_state.indoors || g_settings.outdoors) && LowestBetween(chest, eye, &sightZ);",
       "const bool sight = (g_state.indoors || g_settings.outdoors) && LowestBetween(chest, eye, &sightZ);")]),
    ("a block from outdoors goes on counting in a hall seen from inside",
     [("    } else if (inRoom) {", "    } else if (inRoom && g_state.givenUp) {")]),
    ("the inside view stops when the device refuses the cut",
     [("        // The device does not take the cut. The inside view needs none.\n",
       "        g_state.interior = false;\n")]),
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
