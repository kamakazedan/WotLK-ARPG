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
else, on black (0.4.0). In the game that gave clean rooms, and also city
streets taken for rooms, rooms missing next door, and a band of a fog mod's
fog across the screen. 0.5.0 goes by the game's own rule for what is indoors,
finds the rooms through the doorways itself, and cuts with a clip plane
instead of through the projection. In the game that was right, with two
things left: lamps, planters and passers-by outside the walls still showed
in the black, and the cut under the open sky took the top off every
building on screen. 0.6.0 leaves out what does not stand in a room that is
shown, and under the open sky cut only the parts of a building that were
in the way. In the game that was no better: in a city one "part" is half a
district, and what is cut level shows its inside. 0.7.0 does not cut level
out of doors at all. It takes out of the buildings what covers the
character on screen, along planes through the camera, no wider than the
free space beside the character (not yet through the game). Added since,
from playing it:

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
13. A click walks around what is in the way, up ramps and down stairs,
    instead of in a straight line into it. (Asked for; which of two ways is
    to be decided, see "To be decided: walking around things".)

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

`client/src`, about 3,700 lines of C++ with no dependencies: no Windows SDK, no
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
| `version, features = ARPG_ClientVersion()` | Its presence is how the addon detects the library. `features` is a bit set of the hooks that went in: 1 Lua functions, 2 hold-to-move, 4 camera angles, 8 camera distance, 16 the cutaway (all its hooks), 32 the cutaway shows buildings from inside (0.4.0), 64 it tells rooms from streets and reports what it found (0.5.0), 128 under the open sky it makes an opening along the line of sight and no level cut (0.7.0); the four are set together |
| `ARPG_HoldStart()` / `ARPG_HoldStop()` | The move button went down / came up |
| `ARPG_SetHoldInterval(ms)` | How often a held button re-issues the move (default 100, limits 10 to 2000) |
| `ARPG_SetGroundClicks(maxRise)` | Move clicks that land more than `maxRise` yards above the character's feet go to the ground under the cursor. No argument: off |
| `ARPG_MoveTo(x, y [, z])` | Click-to-move to a spot in world coordinates (x north, y west), as a right click there would. For maps |
| `x, y, z = ARPG_PlayerPosition()` | The character's world position; nothing outside the world |
| `ARPG_SetCamera(yawDeg, pitchDeg, distance [, collide])` | Lock the camera. Yaw 0 looks north, positive turns counter-clockwise; pitch is degrees below the horizon (limit 89); distance in yards (2 to 200); `collide` lets walls shorten the distance |
| `ARPG_SetCamera()` | Release the camera |
| `asked, angleRuns, angleLocked, distanceRuns, distanceLocked = ARPG_CameraInfo()` | Whether a lock is asked for, how often the client has run its two camera routines since the UI loaded, and in how many of those runs the lock was applied. `/arpg status` shows it: a lock that is asked for but never applied is a bug |
| `ARPG_SetCutaway(height [, outdoors])` | While the character is in a room, show the building from inside and nothing else, with whatever is more than `height` yards (2 to 30) above the character's feet left out. With `outdoors` true, also open up, along the line of sight, whatever of a building stands between the camera and the character under the open sky. No argument: off |
| `asked, cutting, height, frames, framesCut, framesSkipped, rooms, indoors, blocked, refused, overhead, between, inside, framesInside, draws, group, groupFlags, area, doorsOut, parts, sceneryOut, creaturesOut, canChoose, canFilter, openLeft, openRight = ARPG_CutawayInfo()` | What the cutaway is doing: whether a cut is on and how far above the feet; scene updates seen, drawn cut, and skipped because the device or its matrices were not as expected; rooms drawn in the last frame; whether the character is in a building (in a room the scene found, or so says the client's indoors test); whether a building blocks the view in the open; whether the cut has been given up because Direct3D refuses its clip plane; how far overhead, and how high between camera and character, the building is (0 for none); whether the last frame was drawn from the character's room; how many frames have been; how many draws of the last cut frame were made with the plane on; the building group under the character (-1 for none), its flags and the area table's flags for it (-1 for no record); and how many places beyond doorways were left out of the last inside frame as not being rooms. Since 0.6.0 also: how many groups of buildings the last cut frame drew with a piece taken out of them for the opening under the open sky (-1 if it was a level cut, for everything; in 0.6.0, the parts found in the way); how much scenery, and how many creatures and objects, the last frame left out (in the inside view for standing outside the rooms shown, under the open sky for standing in the opening); and whether the library can draw building groups in passes and can leave such things out (each needs calls of its own redirected). Under the open sky `height` is how far over the feet the opening's lower edge is. Since 0.7.0 also: how many yards to the left and to the right of the character the opening reaches (0 while there is none). The first fourteen are as in 0.4.0, the first nineteen as in 0.5.0, the first twenty-four as in 0.6.0. `/arpg status` shows it |

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
inside a building it draws the camera's room and what its doorways lead to
(`0x007B3B20`), and draws the outdoor world only as far as that walk came
across doorways and windows to it. And the walls and ceilings of rooms are
made to be seen from inside only, so a camera that is really above the roof
looks straight through the roof and the near walls at the room. What "inside
a building" means is a line straight down from the camera (`0x00795D40`): as
far down as the terrain (`0x007A39F0`), the first building surface it meets
(`0x007D59B0`) names the group the camera is in, unless that group has the
outdoor flag. Over the bank that is the bank's own ceiling. Over most
buildings it is a roof that belongs to the outdoors, and the client draws
the world as usual. Nine redirected calls make the accident the rule
wherever the character is in a room (three more, further down, tell pieces
of the picture apart):

- *Which room.* The library asks the client's two questions itself, from the
  character's chest. If the answer is a room, then for the length of the one
  call that finds the camera's building (`0x00783381`) the scene's camera
  position (`0x00CD8F5C`), which that routine reads once at its start, holds
  the character's chest, and the scene is drawn from that room, even while
  the camera itself hangs above the roof.
- *What a room is.* Not every group without the outdoor flag: 0.4.0 took it
  so, and the first test in the game showed Stormwind's streets, bridges and
  porches drawn "from inside", terrain and water gone and the wall height
  cutting the city. Those are groups of the city's model like any room, with
  doorways between them, only without a ceiling. The client's own answer to
  "is this outdoors" (`0x0077FBF0`, which is behind `IsIndoors()` and where
  one may ride) asks WMOAreaTable.dbc first (`0x00990560`, by the model's
  number, the placement's name set and the group's own number): flag 4 says
  outdoors, flag 2 says indoors, and only without either does the group's
  flag decide. The library asks the same of the group it found, and a room
  is a group that comes out indoors. A group the client draws whenever it is
  in view (flag `0x10000`) is none either.
- *The rooms.* The client's walk (`0x007AD1F0`, `0x007AC060`) is a search
  for what the camera can see: it does not go through a doorway whose far
  side the camera is on, and shows a room beyond a doorway only as far as
  the doorway frames it. From a camera that is not in the room at all that
  left rooms out and showed others in part (0.4.0 widened the frames
  afterwards, but could not bring back what the walk had not reached). The
  walk is not made. In its place (the call at `0x0079AA2C`) every group that
  can be reached from the character's through doorways is found, from the
  doorway lists in the model (`+0x13C`, eight bytes a doorway; each group
  has its run of them at `+0x50`, `+0x54`). A group that counts as outdoors
  is neither shown nor gone through, so the street is not drawn and neither
  is the house across it. Each room that is in view and not wholly above the
  cut is visited (`0x00799310`) under the whole-screen view, which queues
  it, its furniture and, when the client next deals them out
  (`0x00799F80`), the creatures in it.
- *Nothing else.* It is the walk that opens the window onto the outdoor
  world (`0x00ADF58C`), so that stays shut; the window onto the sky
  (`0x00ADF570`), which the client's lookup opens by itself for some rooms,
  is shut the way the client shuts it. The client then draws no sky and
  skips the outdoor world. The screen is cleared to black (the call at
  `0x0079AB9A`).
- *Scenery and creatures.* The scene files each piece of scenery and each
  creature under the groups whose boxes it touches, and a room's box reaches
  past its walls: wherever the walls do not run north to south and east to
  west, well past. The lantern on the outside wall, the planter by the door
  and whoever walks by in the street are filed under the room, and the
  client, going through a visited room for what is in it (`0x00799B70`
  scenery, `0x00793270` creatures and objects; from `0x0079A260`, and from
  `0x00799F80` for groups near the camera), draws them: normally behind a
  wall, here in the black. Both routines ask one question of each piece, at
  one place each (`0x00799C52`, `0x007932F2`): whether its sphere shows in
  the group's view (`0x00983FB0`). Those two calls are redirected. A piece
  the client would draw is asked where it stands, the way the character
  was: the terrain and then the nearest building surface straight under it.
  The line starts half a yard over the piece's centre, since a rug lies on
  the floor and a creature may stand a little sunk into it, and goes 300
  yards down, for what flies or hangs in a high hall. Where it finds
  nothing, the floor is the terrain itself or the piece is set into the
  floor, and the nearest surface straight over the piece says where it is.
  If the answer is not a room that is shown, the client is told the piece
  is out of view. Scenery does not move, so the answer is kept (by the
  piece's address and its centre, since addresses are used again); one
  found in no room is asked about again after 120 frames, in case the floor
  under it had not been loaded. Creatures are asked about every frame,
  except within 8 yards of the character: the character itself, its mount
  and its pet are drawn whatever the lookup says, and so is whoever stands
  in the doorway hitting it, which must not turn invisible for standing on
  a doorstep that counts as outdoors. At most 256 such lookups are made a
  frame; beyond that a piece is drawn unasked.
- *Behind the rooms.* The client clears depth to the far end of the view. A
  shader that fogs the finished picture by depth (CoAVolFog does) finds open
  sky there and fills the black with fog. So in the inside view depth is
  cleared once more, by Direct3D's own `Clear`, to the depth of a wall two
  yards behind the farthest corner of the rooms drawn: nothing drawn lies
  beyond it, and what a fog shader finds is a dark wall at room distance.
- *The cut.* Beams, galleries and the floor of the storey above are solid
  and face the camera. Everything above a level plane is clipped while the
  world is drawn. Up to 0.4.0 the plane took the place of the near side of
  the view (an oblique projection). That clips everything and moves nothing,
  but it changes what depth means, and the same fog shader, reading depth as
  distance, drew a band of fog across the screen wherever the plane was
  close under the camera. Since 0.5.0 the plane is a user clip plane and the
  projection is not touched. With `P` the plane in view space (the world's
  downward direction turned by the view matrix, and the plane's height less
  the camera's) and `m` the hardware projection, the plane in clip space,
  where Direct3D wants it for a draw with a vertex shader, is
  `(P.x / m00, P.y / m11, P.w / m32, P.z - m22 * P.w / m32)`: dotted with a
  vertex as it leaves the shader, that is `P` dotted with the vertex in view
  space again. It goes to Direct3D as plane 5 (`SetClipPlane`; the client
  uses plane 0 for a few models) at `0x0079AB2F`, the last step before
  drawing. Whether it is on is `D3DRS_CLIPPLANEENABLE`, which the client
  sends too, whenever its own choice changes. So the library sets it in the
  one place every draw comes through: the call in the device's draw routine
  (`0x006A3642`, and `0x006A77E2` for the d3d9ex device) to the routine that
  sends Direct3D the engine's states. After that call, for a draw made with
  a vertex shader and with the projection the plane was worked out for, it
  sends the client's own bits plus its own; for any other draw, the client's
  alone. The world's vertex shaders (terrain, buildings, models, liquids,
  particles) all multiply by that projection; a draw without a vertex shader
  would read the plane in another space, and is left uncut. The switch is
  sent for every such draw and not only on change, since another library
  drawing in between may have set its own. The plane is switched off when
  the sorted list of effects has been drawn (the call at `0x004F920B`), the
  last of the world's solid things: the sun's glare, floating text, names
  over heads, a fog shader's passes and the interface come after and are
  drawn whole. A frame that never gets there has it switched off when the
  next one starts.
- *The cursor.* The hit test builds the cursor's ray from the camera
  (`0x004F6450`, called at `0x004F9EC1`), and the roof and the near wall,
  which are not on screen, would stop it. The ray is picked up again where
  it comes through the cut, and then past every surface it meets (found
  with the click's own test, flags `0x100151`) until it starts in a room
  that is drawn, six surfaces at most. A point is in a drawn room if the
  nearest building surface straight below it and the nearest straight above
  it (`0x007D59B0` again) both belong to rooms that were visited. A ray that
  never gets into one is left where the cut put it.

*Outdoors* the client's routine finds what the camera stands in, as ever,
and the client draws from there: also from a street or a square of a city's
model, which it draws properly only from inside. One thing is changed. A
camera that does not stop at walls drifts into rooms the character is not
in, an attic across the street, and from in there the client would draw
that room and what its doorways show, with the character nowhere in the
picture. If the group the camera is in counts as indoors, the scene's record
of it is cleared and the scene is drawn as from outdoors.

*The cut alone.* Where the lookup finds no room although the client's own
indoors test of the character (`0x0071B7F0`) says there is one, the picture
is 0.3.0's: the world is drawn as usual and cut at the plane, and every
indoor group in view and under the plane is visited (walking the list of
placed buildings, `0x00D25440`), so that buildings sliced open are not
empty. It is the rougher picture: whatever is taller than the cut loses its
top, and buildings that are only a shell show it.

*Under the open sky, when asked: an opening along the line of sight.* Two
things were tried before it, and the game showed what was wrong with each.

- 0.5.0 cut level here too: one clip plane for everything. A roof between
  the camera and the character took the top off every building on screen.
- 0.6.0 cut level, but only the groups that were in the way: four lines of
  sight were put to every group about to be drawn, with the client's own
  test of a line against one group (`0x007AE880`, `0x007CB0C0`). That
  worked as written and was no better, because of how the client's files
  divide a city. In Stormwind "the group in the way" is a group that holds
  the fronts of a whole row of houses, the walkways and a bridge. One part
  was cut and half the district came down with it, the raised paths
  included. And a building cut level shows what is in it: nothing, the
  ground far below, or rooms.

Both come from cutting level. A level plane is right where the camera is
meant to look into a room from above; out of doors nothing is meant to be
looked into. What has to go there is exactly what covers the character on
screen, and nothing else has to. So that is what is taken out:

- *Four planes.* Three pass through the camera. One runs down to the level
  line across the view at the character's feet (a quarter of a yard over
  them, so that the ground the character stands on is never above it).
  Two stand upright, through the camera and a point to either side of the
  character: 7 yards from it, or less where something stands beside it
  (below). The fourth stands upright through the character, across the
  view. What lies above the first, between the two side
  planes and on the camera's side of the fourth is the opening: what is in
  it would be drawn over the character, or over the strip of screen beside
  and above it. What is beyond the character can hide nothing of it, and
  is never touched: the wall the character stands in front of keeps its
  top.
- *Nothing to look into.* A plane through the camera is seen edge on. Where
  the first three cut a wall there is no cut face on screen, and no line
  from the camera passes through the cut into the building: a line that is
  above the plane at the camera stays above it, where the building is
  gone, and a line below it stays below, where the building is whole and
  shows its outside. So where those three cut, there is of a building on
  screen either its own outside or nothing, and neither rooms nor the
  hollow under a street come into view. The fourth plane is seen face on.
  A building that stands on both sides of it within the opening is whole
  beyond it and gone on the camera's side, and shows its cut end at the
  character: the archway or the passage the character is under, a house
  right beside the character. That end is not closed with anything.
- *No wider than the free space.* So that as little as possible lies across
  the fourth plane, each side of the opening stops half a yard short of
  the nearest part of a building beside the character. That is looked for
  with a level line to the left and one to the right, 0.6 yards over the
  feet and 7.5 yards long, each tried from both ends like the line of
  sight. Knee-high, because the plane under the opening is close over the
  ground beside the character: without it, steps or a raised pavement
  there would lose a strip along the character's position, as a house wall
  would lose its top. A kerb is lower and is not found. A side is never
  closer than a yard and a half, so that the character is seen whole
  beside a wall it leans on. It comes in at 30 yards a second and goes out
  again at 3: slowly, so that a doorway or the gap between two pillars
  going by does not make the opening twitch. An opening begins as wide as
  it may be.
- *In passes.* Clip planes keep what is on the right side of all of them,
  and what has to be kept here is what is outside any of four. So a group
  that reaches into the opening is drawn up to four times, once for each
  side of the opening that stays: below it (one plane on); above that and
  beyond the character (two); above it, on the camera's side, to the left
  (three); the same to the right (three). Between them these are
  everything but the opening, and no two share more than a plane. Two
  draws that only met at a plane could each leave a point of the screen
  to the other, all along it, so the side below and the side beyond the
  character are drawn 0.02 yards past the plane where the others begin
  (what is see-through is drawn twice in that hair's breadth). The
  client's routine that draws one group (`0x007ABF50`, reached from the
  scene's loop through the one call at `0x007967AA`, which is redirected)
  draws at once and keeps nothing by, so calling it again draws the group
  again; before each call the pass's planes go to Direct3D's planes 5, 4
  and 3 (the client uses plane 0 alone, for a few models). A group's box
  decides whether it reaches into the opening at all, and which passes
  have anything of it in them; most groups are drawn once and whole, as
  ever. Rooms the client has found through a doorway are groups like any
  other here.
- *What stands in it.* A model cannot be cut. The model renderer draws many
  copies of one model with a single draw (`0x00820AE0`: lanterns, barrels,
  fence posts, everything flagged `0x2000` in the model), and a clip plane
  is on or off for a draw. So the two redirected calls that see every
  piece of scenery and every creature of the groups about to be drawn
  leave out the ones whose sphere is wholly in the opening: the sign over
  the door, the chimney, whoever stands on the roof. What reaches out of
  it is drawn whole, and so is any creature within 8 yards of the
  character, as in the inside view. Whether a frame's opening can be made
  at all is known only when its drawing begins, so that goes by the frame
  before: after a frame drawn without, nothing is left out.
- *No rooms.* The level cut without the inside view shows the rooms it
  slices into. Nobody can look into a building through the opening, so no
  room is shown for it.
- *Without the call at `0x007967AA`*, or without the two that see scenery,
  nothing is done under the open sky at all. (Up to 0.6.0 everything was
  cut level then.)

When and where the cut is made is decided once a frame, from whether the
character is in a room, the indoors test, and lines traced through the
world against building surfaces only (`0x0077F310`, flags `0x70`): from the
character's waist straight up and, outside the inside view, from the
character's chest to the camera (from the chest, so that a parapet or a
kerb is not "in the way"). While there is an opening, two more run level
to either side of the character, for how wide it may be.

- In a room, under a roof lower than the camera: always. The cut is the set
  height over the character's feet. Under a ceiling lower than that it is
  just under the ceiling, down to 2 yards over the feet, because a ceiling
  above the cut lies over the room like a lid. In a room whose ceiling is
  more than 3 yards over the set height it is just under the ceiling, so a
  tall room keeps its walls. In the inside view only straight up counts:
  walls there do not hide what is behind them.
- In a hall higher than the camera, seen from inside: not at all.
- Under the open sky: no level cut. The opening, only if asked, and only
  while part of a building stands between camera and character for more
  than 150 ms. It ends 400 ms after the view clears.
- The level cut is never closer than 3 yards over the feet (less only
  under a ceiling), and never with the camera less than a yard above the
  plane. The opening comes down to the feet: it is taken out of buildings
  only, and no draw of the character has its planes on.
- The plane slides: down from the camera when a cut begins, back up when it
  ends, at 60 yards a second. During a cut it comes down at once and goes
  back up only after it could have for a second, and then at 10 yards a
  second. A jump lasts 0.83 seconds, so jumping does not move it, and
  neither does walking under a row of beams. The opening's lower edge
  slides the same way when it begins and ends, down the screen from the
  camera's height to the feet and back; in between it stays with the feet,
  up as well as down, since a line below the feet would take the stairs
  behind the character out with it.
- A cut does not outlive the place it was made for. A level cut is gone
  the moment the character is under the open sky: sliding away, it would
  for a quarter of a second take the top off everything in view, which is
  what is not to happen out there. An opening is gone the moment the
  character is under a roof. Either begins from the top.
- With the camera straight over the character (less than half a yard to
  one side) there is no "between the two", and no opening.
- If the upward line finds nothing indoors, the place counts as enclosed:
  the basic cut does not depend on the trace behaving as read.

What keeps a misreading of the client from doing damage. The inside view
asks the client's own routines the client's own questions and writes values
the client writes itself; all nine calls are redirected or the whole thing
stays off. The three calls for telling pieces apart come on top: without
the two that see scenery and creatures nothing is left out, without those
or the one that sees each group drawn nothing is opened under the open
sky, and either way the rest works as it did in 0.5.0. A group is drawn in
passes only if the matrix the client hands to its draw sits in a placed
building that has this model and this group; otherwise it is drawn as the
client would. Direct3D is only touched if the device is one of the
client's two Direct3D 9 classes (by its virtual table; with OpenGL there
is no cut and the inside view goes on without). The planes are only handed
over if the projection has exactly the shape the client builds for the
world, the view matrix is a plain rotation, and those two put the
character on screen in front of the camera. Whether Direct3D takes all
three of the opening's planes is asked before the frame's drawing, not in
the middle of it. If Direct3D refuses three times the cut is given up,
which the status says. And nothing here is written to the client's
projection, its depth, or its own clip-plane state.

Other libraries. CoAVolFog patches `Wow.exe` too. None of the places it
checks or patches is one of this library's sixteen (its list was read out
of its DLL: the fog routine `0x00873210`, which 0.4.0 detoured, is on it, and is
left alone now; the calls at `0x004F9213` and `0x004FB042`, which would
have been the obvious places to end the cut, are on it as well). It sets
`D3DRS_CLIPPLANEENABLE` to 0 for its own passes.

Known limits. The picture changes at the door, at once. Under a porch roof
that counts as outdoors the roof is drawn, unless the opening under the
open sky is asked for. The opening has hard, straight edges: there is no
fading, and a tower in front of the character is not taken away whole but
has a slot through it. Its lower edge is the line of sight to the
character's feet, so what is on the camera's side of the character and no
higher than that line stays hidden: the ground between the character and
the wall, and whoever stands there. Its sides move when the character
passes something that stands beside it. What it finds beside the
character it finds knee-high: a balcony or an overhanging storey there is
cut into and shows its end. So does whatever the character stands under:
of an archway or a passage through a building the far half is drawn, cut
open where the character is, and rooms the game draws in there can be
seen. Under a roof it shows the ground ahead only as far as the lines of
sight that pass on the camera's side of the character reach: a few yards,
more under a high roof. Scenery in it is all or nothing: a banner that
hangs out of it stays whole, in the air. A creature that shows in it on
the camera's side of the character cannot be clicked: the cursor's ray is
picked up where the opening ends. Only buildings are opened: a tent or a
market stall that is a model, a tree and the terrain hide the character
as before. A creature or an object within 8 yards of the character is drawn
wherever it stands, a mailbox by the door included: creatures and objects
are not told apart here yet. A wall inside the room hides what is behind
it, up to the cut. The level cut takes the tops off creatures taller than
it. Only building surfaces count as overhead, so a
ship's deck or a lift, which are game objects, is not seen by the upward
line. A building whose floor is the terrain itself is not found from inside
(the lookup stops at the terrain). Names of creatures outside may still be
drawn over the black: not yet seen either way. Possible next steps: fade in
and out at the door, the cut end of a building the character is under
closed with a plain colour (its back faces drawn dark where a group is
drawn in passes, which means overriding the client's own states for those
draws), the opening's lower edge taken down to the foot of the wall so
that the ground on the camera's side shows, an opening that reaches
further ahead while the character is under a roof, creatures told from
objects (the callback each carries at `+0x90` is its kind's own), and the
room under the character found the way the client finds a unit's own group
(`0x007A13E0`) where the line down finds none.

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
   cutaway it has a graphics device of the client's Direct3D class with a
   Direct3D object behind it that records what it is asked, buildings with
   rooms, doorways and an area table laid out at the client's offsets, a
   world of floors, ceilings and walls for lines to run into (walls beside
   the character among them, for how wide the opening may be), and a frame
   that goes through the same steps in the same order: the lookup of the
   camera's room, the walk through the doorways, the outdoor world or not,
   the scenery and the creatures of the groups found, the clear, the model
   renderer's passes, the groups one by one, a handful of other draws of
   different kinds, the end of the world's drawing, the interface. Scenery
   and creatures are filed under groups whose boxes take in more than
   their walls. The model renderer runs too, which the library does not
   touch, so that a test can say what each model was drawn with. What was
   "drawn" is judged by putting points through the device's matrices and
   the clip planes each draw had: for the opening under the open sky, a
   point of a group has to be shown by exactly one of the group's draws,
   or by none if it is in the opening. Where the opening is, the tests work
   out for themselves from the camera and the character. It is built
   twice: as a 32-bit Linux program with the library compiled in
   (`client/test/run.sh`), and as a Windows program that loads the real DLL
   (`mock-client.exe`, run by CI on Windows). It is only as good as its
   picture of the client: where the two disagree, the game wins, and the mock
   gets corrected from the disassembly. 0.4.0 passed every check here and
   still drew Stormwind's streets as rooms: the mock had the lookup answer
   "no building" for everything outdoors, and the game does not. Its rooms
   now include a courtyard that is a room by its flags and outdoors by the
   area table. 0.6.0 passed every check too and cut half of Stormwind for
   one house: the mock's buildings were houses, and the game's groups are
   not.
4. `client/test/installer_test.py` - the setup script against a made-up
   `Wow.exe`, under Windows PowerShell 5.1 and PowerShell 7.

Each rule of the cutaway is also broken on purpose, one at a time, to see
that a test notices (`client/test/mutation_test.py`, about half an hour,
run by hand): 250 breakages, from "the plane's sideways term is not divided"
to "a level cut slides away under the open sky". Where one was not noticed,
the tests were extended until it was, or the rule turned out not to be one
and was taken out of the library.

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
| Is this unit outdoors | `0x0071B7F0` -> `0x0077FBF0` | thiscall on the unit. `IsIndoors()` and `IsOutdoors()` (`0x00612300`) are this on the player. In a building group (found by `0x007A13E0` from the groups the unit is filed under): the WMOAreaTable record's flags, 4 outdoors, 2 indoors, else the group's outdoor flag (`0x8`). On terrain: the AreaTable flags |
| WMOAreaTable record | `0x00990560` | cdecl: model number (`+0x20` of the model file's header, which the model points to at `+0x120`), name set (`+0x104` of the placed building), group number (`+0x180` of the group's data). A binary search; null if there is none. Flags at `+0x24` |
| World frame's frame | `0x004FAF90` | saves the device's projection and view, updates (`0x004FA5F0`: camera, then the scene update `0x007831A0`), draws (`0x004F8EA0`), draws names and icons (`0x007E5140`, still with the world's matrices), restores the two matrices and hands the projection to the shaders |
| Drawing the world | `0x004F8EA0` | the model renderer's passes (`0x00823CB0`) around the scene (`0x0077F020` -> `0x00790A80` -> `0x0079A870`), then weather and effects, the sorted list of effects (`0x00681BA0` sorts, `0x00682960` draws; called at `0x004F920B`), the sun's glare (`0x007F0870`), floating text (`0x007E5580`) and the full-screen effects (`0x008C1010`) |
| Device set-up from the camera | `0x00607CB0` | projection and view onto the device; also called by the hit test (`0x004F9E76`) |
| Graphics device | `*0x00C5DF88` | projection as set at `+0xF88`, the hardware one at `+0xFC8`, view matrix stack at `+0x1B00` with its level at `+0x1AF8`. Virtual 39 clears (`0x006A74B0`: colour as given, depth always to 1), 40 sets the projection (Direct3D: `0x006A9B40`), 41 the view. Two Direct3D 9 classes, virtual tables at `0x00A2E718` (d3d9) and `0x00A2F500` (d3d9ex) |
| Direct3D device | `+0x397C` of the graphics device | `IDirect3DDevice9*`. Methods used here: 43 `Clear`, 55 `SetClipPlane`, 57 `SetRenderState` |
| Engine render states | `+0x28F4` of the graphics device | pointer to 0x18-byte records, value first. 18 is the clip-plane mask, 77 the vertex shader (null for a draw without one), 78 the pixel shader. Sent to Direct3D before each draw, each by its own handler (`0x006A4C30`); the clip-plane mask only when it differs from the one last sent, kept at `+0x3E84` (all ones after a reset) |
| A draw | `0x006A3620` (d3d9), `0x006A77C0` (d3d9ex) | thiscall: batch, indexed. Every `DrawPrimitive` and `DrawIndexedPrimitive` the client makes is in these two. Each first calls the routine that sends the states (`0x006A5940` at `0x006A3642`, `0x006A9860` at `0x006A77E2`) |
| Client's own clip plane | `0x00684440`, used at `0x0081FE16` | plane 0, for models flagged for it; the device keeps six planes at `+0x24D4` with a dirty mask at `+0x24D0` and sends them in `0x006A3870` |
| World projection | `0x006BF370` | rows (1/(aspect t), 0, 0, 0), (0, 1/t, 0, 0), (0, 0, (f+n)/(f-n), 1), (0, 0, -2fn/(f-n), 0); row vectors, clip z from -w to w. The Direct3D device stores (m22+1)/2 and m32/2 for the hardware and changes nothing else |
| Projection to the shaders | `0x00872C10` | cdecl, no arguments; from the hardware matrix. 14 callers, the model renderer among them (`0x008230D0`, `0x00823130`) |
| Scene update | `0x007831A0` | view corners `0x00795400` (from the device's matrices, by `0x006BF6D0`), then the camera's building `0x00795D40`, called at `0x00783381` |
| Scene camera position | `0x00CD8F5C` | Vec3 |
| Camera's building, if inside one | `*0x00CD87A4` | null outdoors; a second at `0x00CD87A0`, group lists at `0x00CDB0D4` and `0x00CDB0E4` (capacity, count, pointer to the indexes). Found by `0x00795D40`: a line 1760 yards straight down from the scene camera position, read once at the start, put to the terrain test (`0x007A39F0`, flags `0x100`) and then, as far as the terrain, to `0x007D59B0`. For a group flagged `0x40`, `0x100` or `0x40000` it opens the sky's window there and then |
| Building group on a line | `0x007D59B0` | cdecl: Vec3* from, Vec3* to, float fraction (how far along counts), void* buildings[2], uint32 groups[4]. The group whose surface the line meets first; false if nothing, or if that group has the outdoor flag (`0x8`): a street that is a group without it is found like any room. Groups flagged `0x80`, `0x10000` or `0x400000` are not tested. A line through a doorway gives both groups. Takes any line |
| Walk from the camera's rooms | `0x007B3B20` | thiscall on the placed building: the group list. Called at `0x0079AA2C` (and for the second building at `0x0079A99E`). Visits the camera's groups, then goes through doorways (`0x007AD1F0`, recursing in `0x007AC060`): not through one whose far side the scene camera position is on; a group beyond one is visited under a view narrowed to the doorway; a doorway to a group with the outdoor flag widens the outdoor window. Then visits every group flagged `0x10000` that is in view |
| Doorways | model `+0x13C`, `+0x138` | `+0x13C`: 8 bytes each: surface, group beyond (`0xFFFF` none), side. A group's own run of them: first at `+0x50` of its data, count at `+0x54`. `+0x138`: the surfaces, 20 bytes each |
| Groups near the camera | `0x00799F80` | cdecl: a rectangle. After the walk: goes through the placed groups whose box touches the camera's neighbourhood (list `0x00CDAF48`, filled by `0x007B6110` as it updates placed groups). One whose box touches a group already visited, or any if the outdoor world shows, is visited if it is flagged always-drawn, or walked from if it has the outdoor flag (`0x007B3A10`); then its scenery and creatures are gone through, as below. (Read up to 0.5.0 as dealing creatures out among rooms, which it does not do) |
| Scenery and creatures of the visited groups | `0x0079A260`, called at `0x0079AB2A` | cdecl. For each group on the frame's list (not for one with the outdoor or always-drawn flag unless the camera's building is its own): its scenery (`0x00799B70`) and its creatures and objects (`0x00793270`), kept in lists at `+0x78` and `+0x84` of the placed group. Each piece: model `+0x34` (null while loading), centre of its sphere `+0x38`, radius `+0x44`, once-a-frame stamp `+0xB0`. A piece is put to each of the group's views (`0x00983FB0`, thiscall on the view: centre, radius; called only at `0x00799C52` and `0x007932F2`) until one shows it, and is then queued for the model renderer (`0x00791CB0` scenery; the list `0x00CDB098` creatures, after a callback at `+0x90`) |
| The frame's list of groups to draw | `0x00CDB088`, link offset `0x00CDB080` | placed groups; the link is at `+0xB0`, its second word the next group. The owner link at `+0x20` of a placed group has the placed building at `+8`. (Read by 0.6.0 only) |
| Drawing the groups | `0x007964A0`, called at `0x0079AC35` | takes each group off the list and draws it through `0x007ABF50` (thiscall on the model: group, the placed building's matrix at `+0xB0`, the group's views), called only at `0x007967AA`. That routine draws the group once per view, through a routine picked for the hardware (`0x00CFBEC4`, `0x00CFBECC`), at once: called again it draws the group again, which is how a group is drawn in passes for the opening under the open sky |
| Model renderer | `0x00823CB0` | thiscall on the scene of models: pass. Makes a record on the stack and has `0x00823130` (thiscall on the record: pass, elements, order, count; called at `0x00823D1F` and `0x00823D3B` only) draw a list of elements. Per element: the element at `+0x50` of the record, its model at `+0x60`, the model's lighting at `+0x70`; a call to `0x0081C840` at `0x00823A67`; then one of six routines by the element's kind, each of which sets states for the batch (`0x0081FB10`, where CoAVolFog patches the fog and where the client's own clip plane is switched) and draws. Kind 2 (`0x00820AE0`) draws a run of elements, copies of one model, in a single draw: all scenery whose model has flag `0x2000` at `+0x10` goes that way and no other, which is why one model cannot be given a clip plane of its own. The library touches none of this. Called from the world's drawing at `0x004F911D`, `0x004F916B`, `0x004F91A6`, `0x004F91B9`: the first is one of CoAVolFog's places |
| A line against one building group | `0x007AE880`, `0x007CB0C0` | the first, thiscall on the model (from, to, group): whether a line in the model's coordinates touches the group's box; false for data not loaded. The second, thiscall on the group's data (line of two points, fraction in and out, the line test's flags, the mask of surface kinds to pass over, a byte out, the placed building): whether it meets a surface. The result is whether the hit count at `0x00CB752C` moved; the client zeroes five words from `0x00CB7528` before each call. `0x007AECB0` (thiscall on the model, nine arguments) does both for every group of a model and is what the world's line test uses; `0x007D59B0` does them for the placed groups of every building. `0x007AE140` makes the mask from the flags: `0x82` for `0x70`. (Called by 0.6.0 only: 0.7.0 tells groups apart by their boxes) |
| Loading a group's data | `0x007B6110` -> `0x007AE1A0` -> `0x007D85E0` | by where the group is, not by whether it is seen: a placed group whose box touches the volume kept at `0x00CD779C` (set in the world's update) is loaded if it is not and is not being. A room nobody has looked into loads all the same |
| Windows onto the outdoors | `0x00ADF570`, `0x00ADF58C` | each: left, top, right, bottom as fractions of the screen, then a depth that is below zero when nothing shows. Emptied (FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX, -1) in the scene update (`0x00795400`), and again after the walk of a second building. After the walk the first decides sky, clear colour, weather and light; the second goes to the outdoor culling (`0x0079A790`), skipped if empty |
| Screen clear in the scene draw | `0x006813B0`, called at `0x0079AB9A` | cdecl: what (3: colour and depth), colour. The colour is the sky's or the fog's, by the first window |
| Fog for what is drawn next | `0x00873210` | cdecl: float start, float end, float density, uint32* colour (0xAARRGGBB). Buildings and models set theirs through it; terrain does not. Not patched here: CoAVolFog patches it |
| World switches | `0x00CD774C` | bit `0x100` clear: no buildings drawn, tested or looked up |
| Scene draw | `0x0079A870` | cdecl. Finds what is in view (from the camera's building through doorways, `0x007AD1F0`, or the outdoor world), then `0x0079A260` (scenery and creatures of the visited groups, called at `0x0079AB2A`), `0x00793450` (called at `0x0079AB2F`), clears, has the model renderer work out what it will draw (`0x00821A20`, which is also where each model's lighting callback runs), hands the projection to the shaders, draws the groups (`0x007964A0`) |
| Visit a building group | `0x00799310` | cdecl: group index, placed building. Queues the group once per frame (`+0xB4` of the placed group is its link in the frame's list `0x00CDB080`, nonzero while it is queued) and adds the view on top of the scene's stack as a window onto it. Needs the group's data loaded: no check. Reads `0x00CFBEB8` |
| Box outside the view | `0x0078FB20` | cdecl: Box*; nonzero if wholly outside |
| Placed buildings | list head `0x00D25440`, link offset at `0x00D25438` | per building: flags `+0x0C` (`0x80` groups placed), bounds `+0x48`, model `+0xF4`, name set `+0x104`, group links `+0x11C`, placed groups by index `+0x120`..`+0x130`. Placed group: bounds `+0x24`, index `+0x50`. Model: header `+0x120`, group records `+0x130` (0x20 bytes each, flags first), count `+0x16C`, loaded `+0x1E0`, group data `+0x1F8`. Group data: flags `+0x30`, doorways `+0x50` and `+0x54`, area number `+0x180`, loaded bit at `+0x198`. Group flags: `0x8` outdoor, `0x40` lit by the sky, `0x100` shows the sky, `0x2000` indoor, `0x10000` always drawn |
| UI shutdown | `0x00528F00` | logout, exit and reload. Raises `PLAYER_LOGOUT` (event `0xFE`, `0x0081B530`) at `0x00529025`, then writes the saved settings: bindings, macros, and from `0x0052908E` the CVar caches, each through `0x006B9BE0` (type, writer, argument), which writes the file and then (`0x006B9440`) sends it to the server unless `synchronizeSettings` is 0. The writer for CVars (`0x00512890` -> `0x007670E0`) has no other caller |
| Register a CVar | `0x00767FC0` | cdecl: name, help, flags, default, callback, category, ... Flag `0x10`: saved per account; `0x20`: per character. Event names are a table at `0x00C24EB0` |
| Lua state creation and registration | `0x0052A980` | sets up the UI's scripting (`0x0081A9A0`), then calls `0x005120E0`, which registers the client's functions. Two callers |
| Register a Lua function | `0x00817F90` | cdecl: name, function |
| Last block of the client's Lua registrations | `0x00530F60` | ends in `ret` at `0x00530F85`; tail-called from `0x005120E0` |
| Lua function address check | `0x0086B5A0` | range at `0x00D415B8`/`0x00D415BC`, filled in lazily by `0x0086B510` when either is zero |
| Lua API | | `lua_gettop 0x0084DBD0`, `lua_type 0x0084DEB0`, `lua_tonumber 0x0084E030`, `lua_toboolean 0x0084E0B0`, `lua_pushnumber 0x0084E2A0`, `lua_pushstring 0x0084E350`, `lua_pushnil 0x0084E280` |

## To be decided: walking around things

A click walks in a straight line: the client's own click-to-move, which
follows the ground up a ramp or a flight of stairs that lies on the line,
and stops at a wall, a fence or a crate. Finding a way round takes knowing
where one can walk. There are two places to learn that from.

- **The server.** AzerothCore has navigation meshes for its creatures, and
  mod-playerbots cannot run without them. A small module answers "the way
  from this character to that spot" with the points its own path finder
  gives (`PathGenerator`). The addon asks in an addon message and gets the
  points back; the library walks them, one click-to-move after another, the
  next when the character is within a yard of the last. These are the
  paths the server's creatures take: round obstacles, up stairs, across
  bridges, and as far as the mesh goes, which is what travel by world map
  needs anyway. The price: a module to build into the server, a feature
  that works on that server only, and a question to the server every time
  the destination changes (several a second while the button is held; cheap
  for a server with a handful of players).
- **The client.** The library puts lines to the world itself, with the test
  the camera and the cursor use (`0x007A3B70`, flags `0x100111`: terrain,
  the surfaces of buildings that block movement, and models and objects
  that do), on a grid around the character: straight down for where the
  ground is, level for what stands in the way. A search over that grid
  (A*), spread over a few frames, gives a path. Nothing to install on the
  server and it works anywhere. It is the rougher of the two: it knows only
  what lines can find out, has to guess what the character can step up or
  squeeze through, and sees a screen's width at most.

They share the part that walks a list of points. The first is the better
answer where the server is one's own; the second would make click-to-move
smarter everywhere.

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
