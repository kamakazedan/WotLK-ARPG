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

Goals 1, 2, 3, 5 and 6 are built. Goal 4 is next.

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
| `ARPG_SetCamera(yawDeg, pitchDeg, distance [, collide])` | Lock the camera. Yaw 0 looks north, positive turns counter-clockwise; pitch is degrees below the horizon (limit 89); distance in yards (limit 200); `collide` lets walls shorten the distance |
| `ARPG_SetCamera()` | Release the camera |

### Mechanisms

**Hold-to-move.** The client handles a mouse click in the world in two steps:
on button down it hit-tests the world under the cursor and stores the result
in the world frame (`0x004FA570`); on button up, if the mouse did not drag, it
acts on the stored result (`0x004F7880`, with the button as argument). For
the right button on terrain that ends in the client's own click-to-move; on a
unit or object it targets and interacts, walking into range by itself.

The library hooks the per-frame handler (`0x00495810`) and, while the button
is held, calls those same two routines: once on the press, whatever is under
the cursor, and then every 100 ms as long as terrain is under the cursor.
A held button never re-clicks a unit. If a spell is waiting for a target the
press is delivered as a left click instead, which places the spell or picks
the unit, and nothing repeats. The left mouse button itself is rebound by the
addon to `WOTLKARPG_HOLDMOVE`, so the stock binding that turns the camera
never runs.

Open question until proven in game: whether re-issuing click-to-move ten
times a second looks and feels right (the move marker, stutter). Fallbacks:
call the click-to-move routine directly (`0x00727400`), skip re-issues when
the cursor's ground point has barely moved, or steer by facing.

**Camera lock.** Every frame the client computes the camera's angles
(`0x00604490`), builds its matrix from them, then computes its distance from
the character after collision (`0x00605D60`). The library hooks both. After
the first it overwrites yaw and pitch with the locked values and rebuilds the
matrix (`0x00607BD0`). Around the second it lends the camera the locked
distance and, unless collision is wanted, sets the camera flag that makes the
routine return the wanted distance untouched; everything it changed is put
back when the call returns. So the client's own camera state is never
altered, mouse-look and zoom still change it out of sight, and releasing the
lock shows the client's camera again. Cameras driven by a model animation,
and any camera that is not the world frame's active one, are left alone.

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
   like the client: loads the UI, calls the Lua functions, runs frames, moves
   the camera. It is built twice: as a 32-bit Linux program with the library
   compiled in (`client/test/run.sh`), and as a Windows program that loads the
   real DLL (`mock-client.exe`, run by CI on Windows).
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
| Capture what is under the cursor | `0x004FA570` | thiscall on the world frame, no arguments. Result: kind at `+0x2D8` (0 nothing, 1 terrain, 2 unit or object, 3 building), guid at `+0x2E0`, position at `+0x2E8` |
| Hit-test a screen point | `0x004F9DA0` | thiscall on the world frame: x, y, filter, result. Returns nothing unless bit 0 of the input control's flags (`*0x00C24954 + 0x58`) is set |
| Act on the captured result | `0x004F7880` | thiscall on the world frame: button (1 left, 4 right) |
| Terrain click | `0x00527830` -> `0x00527360` | cdecl, one struct: guid, position, button. Left: a waiting spell takes it (`0x0080C340`). Right: `0x0072B6D0` on the player, then click-to-move `0x00727400` if the `autointeract` CVar is on |
| Unit or object click | `0x00527870` | left `0x005274F0`, right `0x005277B0`, both cdecl (guid) |
| Spell waiting for a target | `*0x00D3F4E4` | non-null while targeting; what `SpellIsTargeting()` tests |
| Object by guid | `0x004D4DB0` | cdecl: guid low, guid high, type mask, file, line |
| Player guid | `0x004D3790` | returns in edx:eax |
| Camera update | `0x00606F90` | called from `0x00607B00`; cameras driven by a model go to `0x00601E90` instead |
| Camera angles | `0x00604490` | thiscall: target, float* yaw, float* pitch, float* roll |
| Camera matrix from angles | `0x00607BD0` | thiscall: yaw, pitch, roll. Matrix at `+0x14`; forward is (cos yaw cos pitch, sin yaw cos pitch, -sin pitch), so yaw 0 is north and positive pitch looks down |
| Camera distance after collision | `0x00605D60` | thiscall, 6 arguments. Returns early with the wanted distance if flag `0x8` is set. World intersection calls at `0x00605F00` and `0x006060FE` |
| Camera fields | | `+0x98` flags (`0x4` model-driven, `0x8` no collision), `+0x118` wanted distance, `+0x1E8` distance blend, `+0x11C` and `+0x12C` yaw terms, `+0x120` pitch, `+0x124` roll |
| World intersect (ray cast) | `0x0077F310` | |
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

## Next: the cutaway

The client draws building interiors by walking portals (doorways, windows)
outward from the camera. A camera above the roof sees no portals, so no rooms
are drawn. Plan: when the character is inside a building or cave, (1) make
the walk treat the character's room as the starting point, (2) clip
everything above head height with a clip plane that follows the character's
height. This is the least certain part of the project.

## Later

- Diablo-style WASD (W moves up the screen), skills aimed at the cursor.
- **Spell.dbc** (client MPQ and server together): facing requirements,
  casting while moving.
- **Server module**: dodge, a pathing service over the server's navigation
  meshes.
