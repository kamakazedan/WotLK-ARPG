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

All six are built. For 4, cutting the whole world open (0.3.0) worked in the
game but showed too much: hollow buildings, the bare ground under a city.
What was asked for after seeing it is the building from inside and nothing
else, on black (0.4.0, not yet through the game). Added since, from playing
it:

7. Move clicks stay at ground level: a click on a roof or a treetop goes to
   the ground under the cursor. (Built.)
8. Sound is heard from the camera, not from the character. (Built.)
9. A chosen skill on the left mouse button, and on the right when WASD is on.
   Clicking an enemy walks into the skill's range and uses it; clicking a
   friend, a quest giver or an object interacts as before. Each has its own
   button on screen next to the action bar. (Next.)
10. A bigger minimap that can be clicked to walk there, and a world map in a
    window that can be clicked to travel there. (Planned.)
11. Nothing here uses or changes another addon: the other addons on the
    client must work exactly as before, for playing the usual way. (A rule.)
12. Nothing of the mode is left in the game's own settings, which follow the
    account to every client it logs in from. (A rule, after 0.3.0 broke it.)

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
- `Camera.lua` - the CVars the mode needs, for a session at a time; the
  locked camera (library) or a saved camera view in slot 5 (no library);
  whether the camera may collide indoors, which it only needs to where there
  is no inside view.
- `Input.lua` - one secure handler frame (`WotLKARPGMoveLock`) owns every
  override binding. Its restricted snippet rebuilds them from attributes, so
  the WASD toggle works in combat. Keys are looked up from the player's real
  bindings, not hard-coded.
- `Options.lua` - the panel under Interface > AddOns. Every control has a
  short label and a tooltip that says what it does in the game. Sliders hold
  whole numbers; one with a wide range snaps while dragged (the rotation
  slider covers 360 values in well under 360 pixels, so a drag alone could
  step over 0), the mouse wheel moves any of them by one, and a right-click
  puts it back to its default. The panel the client gives an addon is about
  400 units wide and 415 high whatever the UI scale, and nothing scrolls:
  the layout is counted out to fit, with the camera sliders two to a row.
- `Bindings.xml` - the mode toggle, the WASD toggle, and `WOTLKARPG_HOLDMOVE`,
  which the left mouse button is pointed at while the mode is on.

The addon owns every setting. The library keeps none: after each UI load the
addon asks it for what it wants. The addon works without the library and
finds out what is there from `ARPG_ClientVersion()`.

Settings are kept for the whole account (`WotLKARPGDB`), and the mode is on
by default, so a new character plays this way from its first login. One
thing stays per character (`WotLKARPGCharDB`) because the client keeps what
it describes per character: whether the saved camera view holds the ARPG
camera. Settings from 0.2.1, which kept everything per character, are
carried over by the first character to log in.

### The game's own settings

The mode needs seven CVars set its way: click-to-move on (`autointeract`,
which the move the library issues goes through), the two camera following
styles at "never", the maximum camera distance at 50, terrain tilt off,
saved views snapping, and the sound listener at the camera. The client
registers the first six per account (flag `0x10` in `0x00767FC0`). Per-account
and per-character CVars are written to `config-cache.wtf` and, unless
`synchronizeSettings` is off, sent to the server, which hands them to
whichever client the account logs in from next. Up to 0.3.0 the mode's
values stayed in place across logouts, so they were saved, and a normal
client on the same account came up with click-to-move on and a camera that
never follows.

The client writes those caches in one place only: when the UI shuts down
(logout, exit, reload), right after it raises `PLAYER_LOGOUT`
(`0x00528F00`: the event at `0x00529025`, the caches from `0x0052908E`).
So the mode's values are a session's: the player's own are noted in memory
when the mode's go in, and put back when the mode is turned off and in the
`PLAYER_LOGOUT` handler; the next login notes them afresh and applies the
mode's again. Nothing is kept in the saved variables. After a crash the
client has written nothing, so there is nothing to undo.

A snapshot left in the saved variables by 0.3.0 or earlier is used once, at
the first login: where a CVar still holds the mode's value, the snapshot has
the player's own; where it holds anything else, the player has set it since,
and that stands.

Rules for addon work:

- Never call `SetBinding` for the mode. Override bindings only.
- Anything that touches bindings or protected frames goes through `ns:Sync()`,
  which waits for combat to end. CVars and the library's camera lock are not
  protected and apply at once.
- Secure snippets may not contain `{`, `}` or the word `function`.
- When the controls switch, call `ns:StopHold()`: the release of a button
  held across the switch may be delivered to a different binding.
- No other addon is used, required, hooked or changed, and no other addon's
  saved variables are read or written. Where another addon already does
  something this one needs (CleanMinimap-WotLK's minimap, BetterMaps-WotLK's
  windowed map), the code is copied in and adapted, so that those addons go
  on working unchanged for playing the usual way.

## Client library

`client/src`, about 2,600 lines of C++ with no dependencies: no Windows SDK, no
C runtime, imports from `kernel32.dll` only. `client/build.sh` builds it with
clang and lld on any OS.

| File | What it is |
|---|---|
| `game.h` | Every client address, offset and calling convention the library relies on |
| `hook.cpp` | Guarded code patching |
| `arpg.cpp` | Movement, the camera lock, the Lua functions, start-up |
| `cutaway.cpp` | Buildings shown from inside, and the cut |
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
  another library's `jmp` at the same spot, or its redirected `call`; the
  hook then goes in front of it and theirs still runs.
- Two kinds of patch: a jump at the start of a routine, and a `call`
  instruction inside a routine pointed somewhere else. The second kind is
  for routines with several callers where only one caller matters.
- All game calls happen on the main thread, from the per-frame hook. The Lua
  functions only set flags, so nothing the game does is ever run from addon
  code.
- The library holds no settings and forgets everything on each UI load.

### Addon-to-library contract

| Lua global | Purpose |
|---|---|
| `version, features = ARPG_ClientVersion()` | Its presence is how the addon detects the library. `features` is a bit set of the hooks that went in: 1 Lua functions, 2 hold-to-move, 4 camera angles, 8 camera distance, 16 the cutaway (all its hooks), 32 the cutaway shows buildings from inside (0.4.0; set together with 16) |
| `ARPG_HoldStart()` / `ARPG_HoldStop()` | The move button went down / came up |
| `ARPG_SetHoldInterval(ms)` | How often a held button re-issues the move (default 100, limits 10 to 2000) |
| `ARPG_SetGroundClicks(maxRise)` | Move clicks that land more than `maxRise` yards above the character's feet go to the ground under the cursor. No argument: off |
| `ARPG_MoveTo(x, y [, z])` | Click-to-move to a spot in world coordinates (x north, y west), as a right click there would. For maps |
| `x, y, z = ARPG_PlayerPosition()` | The character's world position; nothing outside the world |
| `ARPG_SetCamera(yawDeg, pitchDeg, distance [, collide])` | Lock the camera. Yaw 0 looks north, positive turns counter-clockwise; pitch is degrees below the horizon (limit 89); distance in yards (2 to 200); `collide` lets walls shorten the distance |
| `ARPG_SetCamera()` | Release the camera |
| `asked, angleRuns, angleLocked, distanceRuns, distanceLocked = ARPG_CameraInfo()` | Whether a lock is asked for, how often the client has run its two camera routines since the UI loaded, and in how many of those runs the lock was applied. `/arpg status` shows it: a lock that is asked for but never applied is a bug |
| `ARPG_SetCutaway(height [, outdoors])` | While the character is in a building, show the building from inside and nothing else, with whatever is more than `height` yards (2 to 30) above the character's feet left out. With `outdoors` true, also cut the world open wherever a building hides the character from the camera under the open sky. No argument: off |
| `asked, cutting, height, frames, framesCut, framesSkipped, rooms, indoors, blocked, refused, overhead, between, inside, framesInside = ARPG_CutawayInfo()` | What the cutaway is doing: whether a cut is on and how far above the feet; scene updates seen, drawn cut, and skipped because the device's matrices were not as expected; rooms drawn in the last frame; whether the character is in a building (found there by the scene, or so says the client's indoors test); whether a building blocks the view in the open; whether the cut has been given up because the renderer does not keep its projection; how far overhead, and how high between camera and character, the building is (0 for none); whether the last frame was drawn from the character's room; and how many frames have been. `/arpg status` shows it |

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

Where there is no cutaway (switched off, an older library, or a renderer that
refused it) the addon asks for collision while `IsIndoors()` is true (an
option, on by default): a camera above a roof shows roof.

**Inside view.** A camera 20 yards up is fine in the open and useless under
a roof: the client draws the roof. What an ARPG shows there is the room,
looked into from above, and nothing else.

The client can already draw that picture, and in a few buildings it does so
by accident (the Stormwind bank is one). When it takes the camera to be
inside a building it draws from the camera's room outwards through the
doorways (`0x007B3B20`, which judges each doorway with `0x007AD1F0`), and
draws the outdoor world only as far as that walk came across doorways and
windows to it. And the walls and ceilings of rooms are made to be seen from
inside only, so a camera that is really above the roof looks straight
through the roof and the near walls at the room. What "inside a building"
means is a line straight down from the camera (`0x00795D40` asks
`0x007D59B0`): if the first building surface under the camera belongs to an
indoor group, the camera is in that group. Over the bank that is the bank's
own ceiling. Over most buildings it is a roof that belongs to the outdoors,
and the client draws the world as usual. Six redirected calls and one detour
make the accident the rule wherever the character is in a room:

- *The room.* For the length of the one call that finds the camera's
  building (`0x00783381`), the scene's camera position (`0x00CD8F5C`), which
  that routine reads once at its start, holds the character's chest instead.
  The answer is the room the character is in, or none under the open sky,
  even while the camera itself hangs inside somebody's attic. The walk
  through the doorways afterwards still judges each doorway from where the
  camera really is, which leaves out the rooms between camera and character.
- *Nothing else.* The walk leaves behind two rectangles of the screen
  (`0x00ADF570`, `0x00ADF58C`) through which the sky and the outdoor world
  show. Right after the walk (the call at `0x0079AA2C`) both are emptied the
  way the client itself empties them, so it draws no sky and skips the
  outdoor world. The screen is cleared to black (the call at `0x0079AB9A`).
  Fog (`0x00873210`) would fade far walls to the colour of a sky that is not
  there, starting from a camera that is a long way off: it fades to black,
  and starts as far beyond the character as it would have beyond the camera.
  The fog is a detour of its own and a matter of looks only.
- *The rooms' contents.* The client shows the furniture and people of a room
  next door only as far as its doorway frames them. Each group the walk
  queued (`+0xB4` of the placed group) is visited once more (`0x00799310`)
  with the whole screen as its frame, just before the client goes through
  the visited groups (`0x0079AB2A`). A group already queued is not queued
  twice; it only gains the second frame.
- *The cut.* Beams, galleries and the floor of the storey above are solid
  and face the camera. Everything above a level plane is clipped by making
  that plane the near side of the view (Lengyel's oblique near-plane
  projection). With the plane as `P` in view space (the world's downward
  direction turned by the view matrix, and the plane's height less the
  camera's), the depth column of the projection becomes `s*P - (0, 0, 1, 0)`,
  where `s` is chosen so that the far corner of the view on the plane's side
  keeps its depth. x, y and w come out as before, so nothing moves on
  screen. The Direct3D device turns the engine's depth range of -w..w into
  0..w when it takes a projection (`0x006A9B40`); it does so by halving two
  of the four terms and leaving the other two alone, so those two go in
  already halved, and the library reads the hardware matrix back to see that
  the result is half the scaled plane. The swap is made at `0x0079AB2F`, the
  last step before drawing: the client decides what is in view from the
  corners of the view volume, which an oblique projection would wreck, so it
  must not see the changed matrix before then. The shaders are handed the
  new matrix (`0x00872C10`); the model renderer sets itself up after that
  point and takes the device's projection as it finds it, so models are cut
  at the same plane with the same depth. The client's own restore at the end
  of the frame undoes the swap. This part ran in the game in 0.3.0.
- *The cursor.* The hit test builds the cursor's ray from the camera
  (`0x004F6450`, called at `0x004F9EC1`), and the roof and the near wall,
  which are not on screen, would stop it. The ray is picked up again where
  it comes through the cut, and then past every surface it meets (found
  with the click's own test, flags `0x100151`) until it starts in a room
  that is drawn, six surfaces at most. A point is in a drawn room if the
  nearest building surface straight below it and the nearest straight above
  it (`0x007D59B0` again) both belong to groups the walk reached. A ray that
  never gets into one is left where the cut put it.

*The cut alone.* Where the lookup finds no room although the client's own
indoors test (`0x0071B7F0`) says the character is in one, and outdoors when
asked, the picture is 0.3.0's: the world is drawn as usual and cut at the
plane. The scene is told the camera stands in no building, so that the
outdoor world is drawn whatever attic the camera hangs in, and every indoor
group in view and under the plane is visited (walking the list of placed
buildings, `0x00D25440`), so that buildings sliced open are not empty. It is
the rougher picture: whatever is taller than the cut loses its top, and
buildings that are only a shell show it.

When and where the cut is made is decided once a frame, from the scene's
lookup, the indoors test, and lines traced through the world against
building surfaces only (`0x0077F310`, flags `0x70`): from the character's
waist straight up and, outside the inside view, from the character's chest
to the camera (from the chest, so that a parapet or a kerb is not "in the
way").

- In a room, under a roof lower than the camera: always. The cut is the set
  height over the character's feet. Under a ceiling lower than that it is
  just under the ceiling, down to 2 yards over the feet, because a ceiling
  above the cut lies over the room like a lid. In a room whose ceiling is
  more than 3 yards over the set height it is just under the ceiling, so a
  tall room keeps its walls. In the inside view only straight up counts:
  walls there do not hide what is behind them.
- In a hall higher than the camera, seen from inside: not at all.
- Under the open sky: only if asked, and only while part of a building
  stands between camera and character for more than 150 ms; then just under
  the lowest part of it. The cut ends 400 ms after the view clears.
- Never closer than 3 yards over the feet (less only under a ceiling), and
  never with the camera less than a yard above the plane.
- The plane slides: down from the camera when a cut begins, back up when it
  ends, at 60 yards a second. During a cut it comes down at once and goes
  back up only after it could have for a second, and then at 10 yards a
  second. A jump lasts 0.83 seconds, so jumping does not move it, and
  neither does walking under a row of beams.
- If the upward line finds nothing indoors, the place counts as enclosed:
  the basic cut does not depend on the trace behaving as read.

What keeps a misreading of the client from doing damage. The inside view
only ever asks the client's own routines the client's own questions and
writes values the client writes itself; all six calls are redirected or the
whole thing stays off. The projection is only replaced if it has exactly
the shape the client builds for the world, the view matrix is a plain
rotation, those two put the character on screen in front of the camera, and
the camera looks down on the plane. If the device makes anything else of
the matrix than expected, the standard one goes straight back; after three
such frames the cut is given up, which the status says, and the inside view
goes on without it.

Known limits. Rooms on the camera's side of the character's are not shown.
A wall inside the room hides what is behind it, up to the cut. The cut is
level and takes the tops off creatures taller than it. Only building
surfaces count as overhead, so a ship's deck or a lift, which are game
objects, is not seen by the upward line. Names of creatures outside may
still be drawn over the black: not yet seen either way. Possible next
steps: fade in and out instead of switching at the door, exempt models from
the cut (the device has clip planes of its own that only the model renderer
uses), and something better than slicing for the outdoors.

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
   camera updates the way the client's own update routine does. For the
   cutaway it has a graphics device that converts a projection the way the
   Direct3D one does, buildings with rooms and doorways laid out at the
   client's offsets, a world of floors, ceilings and walls for lines to run
   into, and a frame that goes through the same steps in the same order:
   the lookup of the camera's room, the walk through the doorways, the
   outdoor world or not, the clear, the fog; what was "drawn" is judged by
   putting points through the matrices the device ended up with. It is built
   twice: as a 32-bit Linux program with the library compiled in
   (`client/test/run.sh`), and as a Windows program that loads the real DLL
   (`mock-client.exe`, run by CI on Windows). It is only as good as its
   picture of the client: where the two disagree, the game wins, and the mock
   gets corrected from the disassembly.
4. `client/test/installer_test.py` - the setup script against a made-up
   `Wow.exe`, under Windows PowerShell 5.1 and PowerShell 7.

Each rule of the cutaway is also broken on purpose, one at a time, to see
that a test notices (`client/test/mutation_test.py`, a few minutes, run by
hand): 64 breakages, from "the sideways depth term is not halved" to "the
window onto the sky is left open". Several were not noticed at first, and
the tests were extended until they all were.

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
| World intersect (ray cast) | `0x0077F310` -> `0x007A3B70` | cdecl: Vec3* start, Vec3* end, Vec3* out, float* fraction (in: 1.0), flags, 0. Returns whether anything was hit; the hit point is start + (end - start) * fraction. Flags `0x10`, `0x20`, `0x40` select building surfaces (`0x007AE140` turns them into a mask of surface kinds: those that block movement, those that are drawn, less those marked as letting the camera through); `0x100` and others add a second routine for the terrain (`0x007A39F0`); bits in `0x40F0000F` add models and game objects. The click test uses `0x100151`, the camera `0x100171`, the cutaway `0x70` |
| Is this unit outdoors | `0x0071B7F0` | thiscall on the unit. `IsIndoors()` and `IsOutdoors()` (`0x00612300`) are this on the player |
| World frame's frame | `0x004FAF90` | saves the device's projection and view, updates (`0x004FA5F0`: camera, then the scene update `0x007831A0`), draws (`0x004F8EA0` -> `0x0077EFF0` -> `0x0079A870`), restores the two matrices and hands the projection to the shaders |
| Device set-up from the camera | `0x00607CB0` | projection and view onto the device; also called by the hit test (`0x004F9E76`) |
| Graphics device | `*0x00C5DF88` | projection as set at `+0xF88`, the hardware one at `+0xFC8`, view matrix stack at `+0x1B00` with its level at `+0x1AF8`. Virtual 40 sets the projection (Direct3D: `0x006A9B40`), 41 the view |
| World projection | `0x006BF370` | rows (1/(aspect t), 0, 0, 0), (0, 1/t, 0, 0), (0, 0, (f+n)/(f-n), 1), (0, 0, -2fn/(f-n), 0); row vectors, clip z from -w to w. The Direct3D device stores (m22+1)/2 and m32/2 for the hardware and changes nothing else |
| Projection to the shaders | `0x00872C10` | cdecl, no arguments; from the hardware matrix. 14 callers, the model renderer among them (`0x008230D0`, `0x00823130`) |
| Scene update | `0x007831A0` | view corners `0x00795400` (from the device's matrices, by `0x006BF6D0`), then the camera's building `0x00795D40`, called at `0x00783381` |
| Scene camera position | `0x00CD8F5C` | Vec3 |
| Camera's building, if inside one | `*0x00CD87A4` | null outdoors; a second at `0x00CD87A0`, group lists at `0x00CDB0D4` and `0x00CDB0E4` with their counts at `+4`. Found by `0x00795D40`: a line 1760 yards straight down from the scene camera position, read once at the start, put to `0x007D59B0` |
| Building group on a line | `0x007D59B0` | cdecl: Vec3* from, Vec3* to, float fraction (how far along counts), void* buildings[2], uint32 groups[4]. The group whose surface the line meets first; false if nothing, or if that group is an outdoor one. Takes any line |
| Walk from the camera's rooms | `0x007B3B20` | thiscall on the placed building: the group list. Called at `0x0079AA2C`. Judges each doorway from the scene camera position (`0x007AD1F0`) |
| Windows onto the outdoors | `0x00ADF570`, `0x00ADF58C` | each: left, top, right, bottom as fractions of the screen, then a depth that is below zero when nothing shows. Emptied (FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX, -1) in the scene update and again before the walk. After the walk the first decides sky, clear colour, weather and light; the second goes to the outdoor culling (`0x0079A790`), skipped if empty |
| Screen clear in the scene draw | `0x006813B0`, called at `0x0079AB9A` | cdecl: what (3), colour. The colour is the sky's or the fog's, by the first window |
| Fog for what is drawn next | `0x00873210` | cdecl: float start, float end, float density, uint32* colour (0xAARRGGBB). Buildings and models set theirs through it; terrain does not |
| World switches | `0x00CD774C` | bit `0x100` clear: no buildings drawn, tested or looked up |
| Scene draw | `0x0079A870` | cdecl. Finds what is in view (from the camera's building through doorways, `0x007AD1F0`, or the outdoor world), then `0x0079A260` (scenery and creatures of the visited groups, called at `0x0079AB2A`), `0x00793450` (called at `0x0079AB2F`), clears, sets up the model renderer, hands the projection to the shaders, draws (`0x007964A0`) |
| Visit a building group | `0x00799310` | cdecl: group index, placed building. Queues the group once per frame (`+0xB4` of the placed group is nonzero while it is queued) and adds the view on top of the scene's stack as a window onto it. Needs the group's data loaded: no check. Reads `0x00CFBEB8` |
| Box outside the view | `0x0078FB20` | cdecl: Box*; nonzero if wholly outside |
| Placed buildings | list head `0x00D25440`, link offset at `0x00D25438` | per building: flags `+0x0C` (`0x80` groups placed), bounds `+0x48`, model `+0xF4`, group links `+0x11C`, placed groups by index `+0x120`..`+0x130`. Placed group: bounds `+0x24`, index `+0x50`. Model: group records `+0x130` (0x20 bytes each, flags first), count `+0x16C`, loaded `+0x1E0`, group data `+0x1F8` (loaded bit at `+0x198` of each) |
| UI shutdown | `0x00528F00` | logout, exit and reload. Raises `PLAYER_LOGOUT` (event `0xFE`, `0x0081B530`) at `0x00529025`, then writes the saved settings: bindings, macros, and from `0x0052908E` the CVar caches, each through `0x006B9BE0` (type, writer, argument), which writes the file and then (`0x006B9440`) sends it to the server unless `synchronizeSettings` is 0. The writer for CVars (`0x00512890` -> `0x007670E0`) has no other caller |
| Register a CVar | `0x00767FC0` | cdecl: name, help, flags, default, callback, category, ... Flag `0x10`: saved per account; `0x20`: per character. Event names are a table at `0x00C24EB0` |
| Lua state creation and registration | `0x0052A980` | sets up the UI's scripting (`0x0081A9A0`), then calls `0x005120E0`, which registers the client's functions. Two callers |
| Register a Lua function | `0x00817F90` | cdecl: name, function |
| Last block of the client's Lua registrations | `0x00530F60` | ends in `ret` at `0x00530F85`; tail-called from `0x005120E0` |
| Lua function address check | `0x0086B5A0` | range at `0x00D415B8`/`0x00D415BC`, filled in lazily by `0x0086B510` when either is zero |
| Lua API | | `lua_gettop 0x0084DBD0`, `lua_type 0x0084DEB0`, `lua_tonumber 0x0084E030`, `lua_toboolean 0x0084E0B0`, `lua_pushnumber 0x0084E2A0`, `lua_pushstring 0x0084E350`, `lua_pushnil 0x0084E280` |

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

Both are built inside this addon and shown only while the mode is on. The
stock minimap and world map, and whatever other addons do with them, are
left as they are for playing the usual way.

- Minimap: this addon's own larger one, and a click on it calls `ARPG_MoveTo`
  with the character's position plus the click's offset. Walks in a straight
  line, like any click-to-move. CleanMinimap-WotLK has code worth copying.
- World map: this addon's own window, and a click on it travels there. That
  needs real pathfinding, which the client does not have and the server
  does: a small AzerothCore module that answers "path from here to there"
  from its navigation meshes, and the library following the waypoints.
  BetterMaps-WotLK has code worth copying.

## Further out

- Diablo-style WASD (W moves up the screen), skills aimed at the cursor.
- **Spell.dbc** (client MPQ and server together): facing requirements,
  casting while moving.
- **Server module**: dodge, a pathing service over the server's navigation
  meshes.
