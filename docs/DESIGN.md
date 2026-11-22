# Design

Target: World of Warcraft 3.3.5a, build 12340, 32-bit Windows client, on a
privately run AzerothCore server.

## Goals

1. Hold the left mouse button to move continuously towards the cursor. Never
   turn the camera with the mouse.
2. Camera locked facing north at a set height and angle. No scroll zoom. The
   values change only from the options.
3. Camera does not collide with objects.
4. Inside buildings and caves the world is cut away above the character, so
   the character stays visible from the high camera. Going upstairs raises
   the cut, keeping upper-storey walls low.
5. WASD disabled unless switched on.
6. Everything reversible with one switch.

Goals 1, 2, 3, 5 and 6 are built. Added since, from playing it:

7. Move clicks stay at ground level: a click on a roof or a treetop goes to
   the ground under the cursor. (Built.)
8. Sound is heard from the camera, not from the character. (Built.)
9. A chosen skill on the left mouse button, and on the right when WASD is on.
   Clicking an enemy walks into the skill's range and uses it; clicking a
   friend, a quest giver or an object interacts as before. Each has its own
   button on screen next to the action bar. (Next.)
10. A bigger minimap that can be clicked to walk there, and a world map in a
    window that can be clicked to travel there. (Planned.)

## Three layers

| Layer | Lives in | Does |
|---|---|---|
| Addon (Lua) | `addon/WotLK-ARPG` | Mode switch, settings and options, key and mouse layer, skill buttons, on-screen display |
| Client library (C++ DLL) | `client/` | What the Lua sandbox cannot reach: movement by cursor, camera control, rendering |
| Server | separate module repos | Spell data changes, dodge, pathing service |

### Why the addon is not enough

Found by testing in-game with CVars and bindings alone:

- **Hold-to-move.** The only route to click-to-move is the `TURNORACTION`
  binding: button down starts mouse-look, button up performs the click. The
  functions behind it are protected, so an addon cannot call them or repeat
  the click while the button is held. Holding the button therefore turns the
  camera and the character.
- **Locked camera.** Lua cannot read or set the camera angle. The camera can
  be parked (following styles off, zoom keys disabled, a saved view restored),
  but mouse-look still turns it and nothing pins it to north.
- **Camera collision and rendering** are not exposed to Lua at all.

## Addon

- `Core.lua` - saved variables, `ns:Sync()`, events, `/arpg`, and the few
  lines that talk to the library.
- `Camera.lua` - CVar snapshot and restore; the locked camera (library) or a
  saved camera view in slot 5 (no library).
- `Input.lua` - one secure handler frame (`WotLKARPGMoveLock`) owns every
  override binding. Its restricted snippet rebuilds them from attributes, so
  the WASD toggle works in combat. Keys are looked up from the player's real
  bindings, not hard-coded.
- `Options.lua` - the camera panel under Interface > AddOns.
- `Bindings.xml` - the mode toggle, the WASD toggle, and `WOTLKARPG_HOLDMOVE`,
  which the left mouse button is pointed at while the mode is on.

The addon owns every setting. The library keeps none: after each UI load the
addon asks it for what it wants. The addon works without the library and
finds out what is there from `ARPG_ClientVersion()`.

Rules for addon work:

- Never call `SetBinding` for the mode. Override bindings only.
- Anything that touches bindings or protected frames goes through `ns:Sync()`,
  which waits for combat to end. CVars and the library's camera lock are not
  protected and apply at once.
- Secure snippets may not contain `{`, `}` or the word `function`.
- When the controls switch, call `ns:StopHold()`: the release of a button
  held across the switch may be delivered to a different binding.
- Changes that belong to another addon (for example the minimap overlay in
  CleanMinimap-WotLK) are made in that addon's repository.

## Client library

`client/src`, about 1,000 lines of C++ with no dependencies: no Windows SDK, no
C runtime, imports from `kernel32.dll` only. `client/build.sh` builds it with
clang and lld on any OS.

| File | What it is |
|---|---|
| `game.h` | Every client address, offset and calling convention the library relies on |
| `hook.cpp` | Guarded code patching |
| `arpg.cpp` | The features, the Lua functions, start-up |
| `log.cpp` | `WotLK-ARPG.log` next to `Wow.exe` |
| `crt.cpp`, `win.h` | The handful of runtime and Windows declarations a freestanding build needs |

### Loading

`Wow.exe` has to load the library at start-up. Clients prepared for
awesome_wotlk already carry a loader for exactly this: the first instruction
of the routine that runs the game's main function (`0x0040B7D0`, the start of
the main fiber) jumps to a small stub (`0x004E5CB0`, written over the unused
Scan.dll start-up routine) that calls `LoadLibraryA` on a name stored at
`0x004E5CE0`, then carries on. A missing DLL is not an error.

`client/install/WotLK-ARPG-Setup.ps1` changes that name from
`AwesomeWotlkLib.dll` to `WotLK-ARPG.dll`: 20 bytes, written in place, after
a backup, only if the stub is exactly one of the two known forms (the original
one, and St0ny's, which calls `LoadLibraryA` through the import table). The
library in turn loads `AwesomeWotlkLib.dll` first if that file exists, so
awesome_wotlk keeps working. Uninstalling writes the old name back.

The library does not add the stub itself. A client without one needs St0ny's
AIO patcher or awesome_wotlk's patcher first.

### Rules

- Every address is checked against the actual executable before use
  (`client/tools/verify_sites.py` does it for a given `Wow.exe`).
- The library does nothing unless the process is `Wow.exe` build 12340 at its
  usual address.
- Every code patch is guarded: if the bytes at a site are not the expected
  ones, that feature stays off and the reason goes to the log. An unexpected
  executable must never crash. The one foreign patch that is understood is
  another library's `jmp` at the same spot; the hook then goes in front of it
  and theirs still runs.
- All game calls happen on the main thread, from the per-frame hook. The Lua
  functions only set flags, so nothing the game does is ever run from addon
  code.
- The library holds no settings and forgets everything on each UI load.

### Addon-to-library contract

| Lua global | Purpose |
|---|---|
| `version, features = ARPG_ClientVersion()` | Its presence is how the addon detects the library. `features` is a bit set of the hooks that went in: 1 Lua functions, 2 hold-to-move, 4 camera angles, 8 camera distance |
| `ARPG_HoldStart()` / `ARPG_HoldStop()` | The move button went down / came up |
| `ARPG_SetHoldInterval(ms)` | How often a held button re-issues the move (default 100, limits 10 to 2000) |
| `ARPG_SetGroundClicks(maxRise)` | Move clicks that land more than `maxRise` yards above the character's feet go to the ground under the cursor. No argument: off |
| `ARPG_MoveTo(x, y [, z])` | Click-to-move to a spot in world coordinates (x north, y west), as a right click there would. For maps |
| `x, y, z = ARPG_PlayerPosition()` | The character's world position; nothing outside the world |
| `ARPG_SetCamera(yawDeg, pitchDeg, distance [, collide])` | Lock the camera. Yaw 0 looks north, positive turns counter-clockwise; pitch is degrees below the horizon (limit 89); distance in yards (2 to 200); `collide` lets walls shorten the distance |
| `ARPG_SetCamera()` | Release the camera |
| `asked, angleRuns, angleLocked, distanceRuns, distanceLocked = ARPG_CameraInfo()` | Whether a lock is asked for, how often the client has run its two camera routines since the UI loaded, and in how many of those runs the lock was applied. `/arpg status` shows it: a lock that is asked for but never applied is a bug |

Functions added after 0.2.0 are detected by their presence, not by a feature
bit.

### Mechanisms

**Hold-to-move.** The client handles a mouse click in the world in two steps:
on button down it hit-tests the world under the cursor and stores the result
in the world frame (`0x004FA570`); on button up, if the mouse did not drag, it
acts on the stored result (`0x004F7880`, with the button as argument). For
the right button on terrain that ends in the client's own click-to-move; on a
unit or object it targets and interacts, walking into range by itself.

The library hooks the per-frame handler (`0x00495810`) and, while the button
is held, calls those same two routines: once on the press, whatever is under
the cursor, and then every 100 ms. A press on a unit or object is one
interaction and the rest of that hold does nothing, so that a creature
walking out from under the cursor cannot call the character off. A press on
the ground starts a walk, which repeats; a walk never clicks a unit it passes
over. If a spell is waiting for a target the press is delivered as a left
click instead, which places the spell or picks the unit, and nothing repeats.
The left mouse button itself is rebound by the addon to `WOTLKARPG_HOLDMOVE`,
so the stock binding that turns the camera never runs.

**Ground clicks.** The capture also stores the cursor's ray, from the camera
to the far end (world frame `+0x2F8` and `+0x304`). The static world is one
kind of hit, whether terrain, building or tree, so a roof cannot be told from
a hillside by kind. What the library does instead: if the hit is more than
`maxRise` (3 yards) above the character's feet, it replaces the stored hit
position with the point where the ray passes through the height of the feet,
before the click is acted on. During a walk the same point is used to keep
going while the cursor is over a unit (through the terrain-click routine,
`0x00527830`). On foot, click-to-move judges arrival by x and y alone, so the
height of that point does not have to match the real ground. The cost is on
slopes: a click far up a hillside is treated like a roof and lands further up
the screen. Possible refinement: cast the ray on from the hit (world
intersect `0x0077F310`, flags `0x100151` as the client's own click test uses)
and only treat the hit as a roof if something lies beneath it.

**Camera lock.** Every frame the client's camera update (`0x00606F90`)
computes the camera's angles (`0x00604490`, which also builds the matrix),
then its distance from the character after collision (`0x00605D60`), then
places the camera that far behind the character along the matrix's forward
axis (`0x00601D60`). The library hooks the first two.

- Angles: after the client's routine, yaw and pitch are overwritten with the
  locked values and the matrix is rebuilt (`0x00607BD0`). The client's own
  yaw and pitch are not touched: mouse-look still changes them, out of sight,
  and they are what shows again when the lock is released.
- Distance: the camera's own zoom (`+0x118`, and the zoom to return to after
  a collision, `+0x1E8`) is kept at the locked distance for as long as the
  lock lasts, and the player's zoom is put back afterwards. Lending the
  distance for the one call is not enough: other parts of the client ask the
  zoom, not the camera's position, how close the camera is. Below 1.83 yards
  they treat the view as first person (`0x005FE3A0`), and the update itself
  compares zoom and distance to detect a collision.
- No collision: the distance routine returns the zoom untested while the
  camera's first-person flag (`0x8`) is set. The library raises the flag for
  that one call and lowers it before the caller looks, because the caller
  would otherwise use a distance of zero.

Only the world frame's active camera is touched. Cameras driven by a model
(cinematics) go through a different update and never reach these routines.

0.2.0 shipped with the lock dead. `game.h` had flag `0x4` down as "driven by
a model" and skipped any camera that had it set; it is in fact set on every
camera once it has been set up on its target. The mock client shared the
misreading, so the tests passed. The mock now reproduces the client's camera
update step by step, flags included, and `/arpg status` reports from the
library's own counters whether the lock is being applied.

Until the cutaway exists the addon asks for collision while `IsIndoors()` is
true (an option, on by default): a camera above a roof shows roof.

**Lua functions.** The client registers its own functions at each UI load;
the last block of them is registered by a routine ending at `0x00530F85`.
That `ret` is replaced by a jump to a thunk that registers the library's
functions and then returns. The client refuses to call C functions whose
address is outside its own code, checked against a range kept at
`0x00D415B8`/`0x00D415BC`; the range is widened at start-up.

### Testing

Four things stand in for the game, which CI cannot run:

1. `client/tools/verify_sites.py Wow.exe` - the patch sites in `game.h`
   against a real executable. Run it against your own file; CI has none.
2. `client/test/hook_test.cpp` - the patching engine alone, on hand-assembled
   functions: stock bytes, unexpected bytes, another library's hook in place.
3. `client/test/mock_client.cpp` - a stand-in for the client. It lays out
   memory at the client's addresses, puts a routine with the right calling
   convention (and the client's exact first bytes, where the library patches)
   at each address the library uses, lets the library start, and then acts
   like the client: loads the UI, calls the Lua functions, runs frames, runs
   camera updates the way the client's own update routine does. It is built
   twice: as a 32-bit Linux program with the library compiled in
   (`client/test/run.sh`), and as a Windows program that loads the real DLL
   (`mock-client.exe`, run by CI on Windows). It is only as good as its
   picture of the client: where the two disagree, the game wins, and the mock
   gets corrected from the disassembly.
4. `client/test/installer_test.py` - the setup script against a made-up
   `Wow.exe`, under Windows PowerShell 5.1 and PowerShell 7.

None of this shows that the real client behaves like the mock. That takes
the game.

### Client internals map

Verified by disassembling the 12340 executable. Addresses are for the image
at `0x00400000`.

| What | Address | Notes |
|---|---|---|
| Start of the main fiber | `0x0040B7D0` | stdcall, 1 argument; calls the game's main routine `0x00406D70`. Stock bytes `55 8B EC E8 98 B5 FF FF` |
| Loader stub (when present) | `0x004E5CB0` | DLL name at `0x004E5CE0`, 20 bytes; file offset `0xE50E0` |
| Per-frame handler | `0x00495810` | cdecl, 4 arguments (the 4th a float). Updates the UI (`0x00495320`), then draws it (`0x00495410`) |
| UI root | `*0x00B499A8` | |
| In-world flag | `0x00BD0792` | byte |
| World frame | `*0x00B7436C` | active camera pointer at `+0x7E20` |
| Capture what is under the cursor | `0x004FA570` | thiscall on the world frame, no arguments. Result: kind at `+0x2D8`, guid at `+0x2E0`, hit position at `+0x2E8`, the cursor's ray at `+0x2F8` (start) and `+0x304` (end) |
| Kinds of hit | | 0 nothing; 1 the static world (terrain, buildings, trees alike); 2 a unit or object to interact with; 3 the solid shape of a game object (a ship's deck). A hit on a transport carries its guid and a position in the transport's coordinates |
| Cursor ray from screen coordinates | `0x004F6450` | thiscall on the world frame: x, y, Vec3* start, Vec3* end |
| Hit-test a screen point | `0x004F9DA0` | thiscall on the world frame: x, y, filter, result. Returns nothing unless bit 0 of the input control's flags (`*0x00C24954 + 0x58`) is set |
| Act on the captured result | `0x004F7880` | thiscall on the world frame: button (1 left, 4 right) |
| Terrain click | `0x00527830` -> `0x00527360` | cdecl, one struct: transport guid or zero, position, button. Left: a waiting spell takes it (`0x0080C340`). Right: `0x0072B6D0` on the player, then click-to-move `0x00727400` if the `autointeract` CVar is on |
| Click-to-move | `0x00727400`, per-frame `0x007317A0` | thiscall on the player: action (4 walk to a spot, 13 none), guid*, position*, float. State at `0x00CA11F4` (action) and `0x00CA1264` (destination). No distance limit for action 4. On foot, arrival is judged in x and y only; swimming or flying, in all three. Can give up after five updates without getting closer |
| Unit or object click | `0x00527870` | left `0x005274F0`, right `0x005277B0`, both cdecl (guid) |
| Spell waiting for a target | `*0x00D3F4E4` | non-null while targeting; what `SpellIsTargeting()` tests |
| Object by guid | `0x004D4DB0` | cdecl: guid low, guid high, type mask (`0x10` player, `0x8` unit, `0x1` any), file, line |
| Player guid | `0x004D3790` | returns in edx:eax; reads thread-local data |
| Object virtuals | | slot 11 (`+0x2C`): Vec3* GetPosition(Vec3* out); slot 13 (`+0x34`): float GetFacing() |
| Camera update | `0x00606F90` | thiscall: target, time. Called from `0x00607B00`; cameras driven by a model go to `0x00601E90` instead |
| Camera position from pivot and distance | `0x00601D60` | thiscall: Vec3* out, Vec3* pivot, float distance, Vec3* extra. Result goes to the camera's `+0x08` |
| First-person test used outside the camera code | `0x005FE3A0` | zoom below 1.8315 |
| Camera angles | `0x00604490` | thiscall: target, float* yaw, float* pitch, float* roll |
| Camera matrix from angles | `0x00607BD0` | thiscall: yaw, pitch, roll. Matrix at `+0x14`; forward is (cos yaw cos pitch, sin yaw cos pitch, -sin pitch), so yaw 0 is north and positive pitch looks down |
| Camera distance after collision | `0x00605D60` | thiscall, 6 arguments. Starts from the larger of zoom and zoom blend. Returns flag bits that the caller ORs into the flags. Returns early with the zoom if flag `0x8` is set. World intersection calls at `0x00605F00` and `0x006060FE` |
| Camera fields | | `+0x08` position, `+0x14` matrix, `+0x98` flags (`0x4` set up on its target, the normal state; `0x8` first person), `+0x118` zoom, `+0x1E8` zoom to return to after a collision, `+0x11C` and `+0x12C` yaw terms, `+0x120` pitch, `+0x124` roll |
| World intersect (ray cast) | `0x0077F310` | cdecl: Vec3* start, Vec3* end, Vec3* out, float* fraction (in: 1.0), flags, 0. Returns whether anything was hit. The click test uses flags `0x100151` |
| Lua state creation and registration | `0x0052A980` | sets up the UI's scripting (`0x0081A9A0`), then calls `0x005120E0`, which registers the client's functions. Two callers |
| Register a Lua function | `0x00817F90` | cdecl: name, function |
| Last block of the client's Lua registrations | `0x00530F60` | ends in `ret` at `0x00530F85`; tail-called from `0x005120E0` |
| Lua function address check | `0x0086B5A0` | range at `0x00D415B8`/`0x00D415BC`, filled in lazily by `0x0086B510` when either is zero |
| Lua API | | `lua_gettop 0x0084DBD0`, `lua_type 0x0084DEB0`, `lua_tonumber 0x0084E030`, `lua_toboolean 0x0084E0B0`, `lua_pushnumber 0x0084E2A0`, `lua_pushstring 0x0084E350`, `lua_pushnil 0x0084E280` |

Not yet verified, from the awesome_wotlk projects' headers, for the cutaway:

| What | Address | Notes |
|---|---|---|
| World render | `0x0079A870` | cdecl |
| Building walk from camera | `0x007AD1F0` | thiscall on the map object |
| Building walk from outside | `0x007AD350` | thiscall on the map object |
| Visit one building group | `0x007A6B60` | thiscall: group index |
| Camera's building, if inside one | `*0x00CD87A4` | null outdoors |
| Graphics device draw | `0x006A3620` | thiscall on the D3D device wrapper |

## Next: skills on the mouse buttons

A press on an enemy should use a chosen skill, walking into range first. The
pieces:

- Addon: two skill slots (left button; right button when WASD is on), shown
  as extra buttons by the action bar, filled by dragging a spell onto them.
  At the press, the addon knows whether the unit under the cursor is an enemy
  (`UnitCanAttack("player", "mouseover")`) and the skill's range
  (`GetSpellInfo`).
- Library: casting and targeting are protected from addon code, so the
  library has to do them: target the unit that was captured, walk towards it
  until the skill is in range, stop, cast. The simplest route is to run a
  line of Lua from the frame hook, where nothing is tainted, rather than to
  call the client's spell code directly.

## Then: maps

- Minimap (in CleanMinimap-WotLK): larger, and a click on it calls
  `ARPG_MoveTo` with the character's position plus the click's offset. Walks
  in a straight line, like any click-to-move.
- World map (in BetterMaps-WotLK): a window, and a click on it travels there.
  That needs real pathfinding, which the client does not have and the server
  does: a small AzerothCore module that answers "path from here to there"
  from its navigation meshes, and the library following the waypoints.

## Later: the cutaway

The client draws building interiors by walking portals (doorways, windows)
outward from the camera. A camera above the roof sees no portals, so no rooms
are drawn. Plan: when the character is inside a building or cave, (1) make
the walk treat the character's room as the starting point, (2) clip
everything above head height with a clip plane that follows the character's
height. This is the least certain part of the project.

## Further out

- Diablo-style WASD (W moves up the screen), skills aimed at the cursor.
- **Spell.dbc** (client MPQ and server together): facing requirements,
  casting while moving.
- **Server module**: dodge, a pathing service over the server's navigation
  meshes.
