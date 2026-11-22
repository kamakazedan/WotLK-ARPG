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

## Addon (v0.1)

- `Core.lua` - saved variables, `ns:Sync()`, events, `/arpg`.
- `Camera.lua` - CVar snapshot and restore, saved camera view (slot 5).
- `Input.lua` - one secure handler frame (`WotLKARPGMoveLock`) owns every
  override binding. Its restricted snippet rebuilds them from attributes, so
  the WASD toggle works in combat. Keys are looked up from the player's real
  bindings, not hard-coded.

Rules for addon work:

- Never call `SetBinding` for the mode. Override bindings only.
- Anything that touches bindings or protected frames goes through `ns:Sync()`,
  which waits for combat to end.
- Secure snippets may not contain `{`, `}` or the word `function`.
- Changes that belong to another addon (for example the minimap overlay in
  CleanMinimap-WotLK) are made in that addon's repository.

## Client library (planned)

### Loading

A one-time patch to `Wow.exe` makes it load the library at start-up. This is
the mechanism the awesome_wotlk projects use (the unused Scan.dll start-up
stub is replaced with a `LoadLibrary` call). The patcher will:

- refuse to touch a file whose bytes at the patch sites are not the expected
  stock bytes,
- keep a backup (`Wow.exe.bak`) and offer to restore it,
- chain-load `AwesomeWotlkLib.dll` when a client is already patched for it.

### Rules

- Every address is checked against the actual executable before use.
- Every code patch is guarded: if the bytes at a site are not the expected
  ones, that feature stays off and the reason is written to a log file next
  to `Wow.exe`. An unexpected executable must never crash.
- All game calls happen on the main thread, from the per-frame hook.
- The library holds no settings. The addon owns them and pushes them in.

### Addon-to-library contract (draft, will change)

| Lua global | Purpose |
|---|---|
| `ARPG_ClientVersion()` | Returns the library version. Its presence is how the addon detects the library |
| `ARPG_SetCamera(yawDeg, pitchDeg, distance)` | Lock the camera to these values, or pass nothing to release it |
| `ARPG_SetCameraCollision(enabled)` | Camera collision on or off |
| `ARPG_HoldStart()` / `ARPG_HoldStop()` | Called from the left-button binding (key down and key up) |
| `ARPG_CursorWorldPosition()` | World x, y, z under the cursor, for aimed skills |

### Mechanisms

**Hold-to-move.** While the button is held, each frame: hit-test the world
under the cursor with the client's own routine, then hand the point to the
client's own terrain-click handler, which already implements click-to-move.
A unit or object under the cursor on press gets the client's own right-click
action instead (attack, talk, loot), which walks into range by itself.

**Camera lock.** Each frame, before the camera is used for rendering, write
yaw, pitch and distance. Collision: the camera update makes one world
intersection call from the character to the wanted camera position; with
collision off that call reports no hit.

**Cutaway.** The client draws building interiors by walking portals
(doorways, windows) outward from the camera. A camera above the roof sees no
portals, so no rooms are drawn. Plan: when the character is inside a building
or cave, (1) make the walk treat the character's room as the starting point,
(2) clip everything above head height with a clip plane that follows the
character's height. This is the least certain part and is done last.

### Client internals map

From the open-source awesome_wotlk projects (FrostAtom's original and the
FreakySphere build), which run on this exact client build. **Not yet verified
against our executable.**

| What | Address | Notes |
|---|---|---|
| Patch site: Lua `ScanDLLStart` | `0x004DCCF0` | stock bytes `55 8B EC 56 8B 75` |
| Patch site: `ScanDllStart` | `0x004E5CB0` | stock bytes `55 8B EC 56 33 F6 39 35` |
| Patch site: start address | `0x0040B7D0` | stock bytes `55 8B EC E8 98 B5 FF FF` |
| Lua function pointer range check | `0x00D415B8`, `0x00D415BC` | widened so Lua accepts functions outside `Wow.exe` |
| Per-frame UI update | `0x00495810` | `FrameScript` OnUpdate dispatch |
| Lua state | `0x00817DB0` | getter |
| In world flag | `0x00BD0792` | byte |
| World frame | `*0x00B7436C` | camera pointer at `+0x7E20` |
| Active camera | `0x004F5960` | getter |
| Camera fields | `+0x08` position, `+0x14` matrix, `+0x40` FOV, `+0x118` distance, `+0x11C`, `+0x120`, `+0x128` yaw and pitch state | exact roles of the yaw and pitch fields to be confirmed |
| Camera collision call site | `0x006060E6` | call to the world intersect routine |
| World intersect (ray cast) | `0x0077F310` | cdecl: start, end, hit point, hit distance, flags, buffer |
| Hit-test a screen point | `0x004F9DA0` | thiscall on the world frame: x, y, filter, result |
| Terrain click handler | `0x00527830` | cdecl, takes guid, position, button (1 left, 4 right) |
| Unit right-click by guid | `0x005277B0` | cdecl |
| Object by guid | `0x004D4DB0` | cdecl: guid, type mask |
| Player guid | `0x004D3790` | getter |
| Object virtuals | index 11 position, 13 facing, 44 right-click | |
| UI root | `*0x00B499A8` | holds the mouse position and the frame under the mouse |
| World render | `0x0079A870` | cdecl |
| Building walk from camera | `0x007AD1F0` | thiscall on the map object |
| Building walk from outside | `0x007AD350` | thiscall on the map object |
| Visit one building group | `0x007A6B60` | thiscall: group index |
| Camera's building, if inside one | `*0x00CD87A4` | null outdoors |
| Graphics device draw | `0x006A3620` | thiscall on the D3D device wrapper |

## Later

- **Spell.dbc** (client MPQ and server together): facing requirements,
  casting while moving.
- **Server module**: dodge, a pathing service over the server's navigation
  meshes.
