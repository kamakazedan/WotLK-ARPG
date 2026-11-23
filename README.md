# WotLK-ARPG

Diablo-style play for a World of Warcraft 3.3.5a (build 12340) client: a fixed
top-down camera that faces north, hold the left mouse button to move,
buildings you look into from above once you walk inside, and WASD switched
off unless you turn it on. Built for a privately run server.

It comes in two parts:

- **The addon** (`Interface/AddOns/WotLK-ARPG`) holds the settings and the
  controls. On its own it gives click-to-move and a parked camera.
- **The client library** (`WotLK-ARPG.dll`, loaded by `Wow.exe`) does what an
  addon is not allowed to: hold-to-move, a camera that cannot be turned,
  zoomed or pushed around by walls, and buildings shown from inside.

It stands alone. It does not use, need or change any other addon, so the rest
of your addons behave as they always do, with the mode on or off. And it
leaves nothing behind in the game's own settings: what it changes while you
play is put back before the game saves anything, so another client you log
in from, with or without this addon, finds your settings as you left them.

| Feature | Needs | Status |
|---|---|---|
| One switch for the whole mode, fully reversible; on from the first login | addon | done |
| WASD off unless enabled, toggle works in combat | addon | done |
| No scroll zoom, camera stops following the character | addon | done |
| Hold the left mouse button to keep moving; click to interact, attack, loot | client library | works in game |
| Camera locked facing north at a set angle and distance, through walls | client library | works in game |
| Clicks on roofs and treetops move to the ground under the cursor | client library | works in game |
| Sound heard from the camera, so left and right match the screen | addon | done |
| Settings panel; settings shared by every character | addon | done |
| The game's own settings as you left them on every other client | addon | fixed in 0.4.0 |
| Indoors, the building shown from inside, looked into from above | client library | works in game since 0.5.0; with a fog mod the background comes out grey, not black |
| Indoors, walls, beams and upper floors cut off at a set height | client library | works in game |
| Indoors, scenery and creatures that stand outside the rooms left out | client library | new in 0.6.0; not yet run in game |
| Outdoors, the building that hides the character cut open, and nothing else | client library | new in 0.6.0 (before: the whole screen); not yet run in game; off unless asked for |
| A skill on the left mouse button (and the right, with WASD on): walk into range, then cast | addon + client library | next |
| Bigger minimap; click it to walk there | addon + client library | planned |
| Walking around obstacles, up ramps and down stairs to where you clicked | client library + server pathing | planned |
| World map in a window; click a place to travel there | addon + server pathing | planned |
| Health and resource orbs | addon | planned |
| Casting while moving, dodge | server + Spell.dbc | planned |

## Install

You need a 3.3.5a `Wow.exe` that already loads a DLL at start-up: one patched
with [St0ny's AIO patcher](https://github.com/Raz0r1337/St0nys-AIO-WoW-EXE-Patcher)
(patch "Load AwesomeWotlkLib.dll at startup") or with awesome_wotlk's own
patcher. The setup script checks for this and tells you if it is missing.

1. Download the `WotLK-ARPG` build: the artifact of the latest green run under
   **Actions > build**.
2. With the game closed, put its contents into the game folder (next to
   `Wow.exe`). The `Interface` folder merges with the one already there.
3. Double-click `WotLK-ARPG-Install.bat`. It changes which DLL the loader in
   `Wow.exe` asks for (20 bytes), after copying `Wow.exe` to
   `Wow.exe.before-WotLK-ARPG`.
4. Start the game and log in. The mode is on from the first login; `/arpg`
   turns it off and on.

To update, close the game and copy the new files over the old ones. The setup
script does not need to run again.

`WotLK-ARPG-Uninstall.bat` puts the 20 bytes back. Before running any other
patcher on `Wow.exe`, uninstall first and install again afterwards.

Addon only, no client changes: copy `addon/WotLK-ARPG` into `Interface/AddOns/`.

## Use

| Command | What it does |
|---|---|
| `/arpg` | Turn the mode on or off (`/arpg on`, `/arpg off`) |
| `/arpg wasd` | Lock or unlock WASD movement |
| `/arpg options` | All settings (also under Interface > AddOns) |
| `/arpg angle 60` | How steeply the camera looks down, 10 to 89 degrees |
| `/arpg distance 22` | How far the camera is from the character, 5 to 100 yards |
| `/arpg rotation 0` | Turns the whole view; 0 keeps north at the top |
| `/arpg cut` | Buildings shown from inside, on or off; `/arpg cut 4` sets how many yards of wall are left standing; `/arpg cut outdoors` switches the opening up of buildings outdoors |
| `/arpg ground` | Roof and treetop clicks go to the ground under the cursor, on or off; `/arpg ground 3` sets from how many yards up |
| `/arpg sound` | Hear the world from the camera or from the character |
| `/arpg camedit` | Free the camera, mouse and scroll wheel for a look around |
| `/arpg status` | Show what is on, and whether the camera lock and the inside view are really in effect |

Keys for the mode and for the WASD toggle are under **Key Bindings > WotLK-ARPG**.
Bind the WASD toggle there: it is the one that works in combat.

While the mode is on:

- **Left mouse button**: press on the ground to move there, hold to keep
  following the cursor. Press on a creature, player, object or corpse to walk
  up and interact, attack or loot. If a spell is waiting for a target, the
  press gives it one.
- **Roofs and treetops**: from above, the cursor is often on a roof when you
  mean the street or the floor behind it. A move click that lands more than
  3 yards above your character's feet goes instead to the spot where the
  cursor's line of sight reaches your character's own height. On a steep
  hillside that makes a far uphill click land a little further up the screen
  than the cursor; raise the height with `/arpg ground 6`, or switch it off,
  if that gets in the way.
- **Sound** is heard from where the camera is, facing the way it faces, so a
  noise on the right of the screen comes from the right speaker. It is also
  as far away as the camera is; `/arpg sound` switches back.
- **Camera**: stays where the settings put it, 22 yards out at 60 degrees
  unless you change it, and goes through walls instead of zooming in.
- **Indoors** you see the building from inside and nothing else. The roof
  and the walls on the camera's side are left out, so you look into the
  rooms from above; the street, the hills and the sky are not drawn at all,
  and the screen is black where the building ends. Every room you could
  walk to from yours without going outdoors is shown, whole. Whatever is
  more than 4 yards above your feet is cut away: beams, galleries, the floor
  upstairs and the tops of the walls. Under a ceiling lower than that the
  cut is made just below the ceiling, and a tall room keeps its walls up to
  its ceiling. Clicks and hover go through everything that is left out.
  Upstairs, the cut goes up with you. The lamp outside the door, the
  planter under the window and whoever walks by in the street are not drawn
  either, although the game counts them to the room; only what is within
  8 yards of your character always is, so that nobody who can hit you is
  invisible for standing on the doorstep.
- **What counts as indoors** is the game's own answer, the one that decides
  where you may ride. A city's streets, squares and bridges are outdoors,
  although the game builds many of them as part of the same model as the
  houses; so are most porches and gateways. You step from one picture into
  the other at the door.
- **Outdoors** nothing changes, unless you ask for it: with "Outdoors, open
  up the building that hides you" ticked, a roof, an archway or a wall that
  comes between the camera and your character is cut away just under where
  it hides you. Only the parts of the building that are in the way are cut,
  with the rooms inside them; the signs, lanterns and chimneys that sit on
  those parts above the cut are left out with them. Other buildings, the
  street, the terrain and the trees stay whole. It keeps the character in
  view under an archway or a porch roof and behind a house. A part goes
  back to whole a second after it stops being in the way.
- **Scroll wheel and right mouse button**: do nothing, for now.

In the options, drag a slider or roll the mouse wheel over it for single
steps; right-click puts it back to its default.

The settings are shared by every character on the account, and the mode is on
when you log in unless you switched it off. Turning it off puts everything
back. The addon never changes your saved key bindings.

### Your other clients

The game keeps some settings per account (camera following style, maximum
camera distance, follow terrain, click-to-move) and stores them on the server,
so that they follow you to any client you log in from. The mode needs six of
them set its own way while it is on. It puts your own values back whenever
you log out, exit or reload the UI, before the game saves them, and applies
its own again when you log in. Up to 0.3.0 it did not, and the mode's values
turned up on every other client of the account. Logging out once with 0.4.0,
on the character you first used the mode with, puts your old values back;
or set them once by hand on the other client (Interface > Camera, and
Click-to-Move under Mouse), and from then on they stay.

Without the client library the addon still works: one click per move,
`/arpg camedit` and `/arpg savecam` to set the camera up by hand.

### What the inside view cannot do yet

- The picture changes at the door, at once: there is no fade yet.
- Under a porch roof or an archway that the game counts as outdoors you are
  outdoors, and the roof is drawn unless "Outdoors, open up the building
  that hides you" is ticked.
- Outdoors a building is cut open in whole parts, as the game's files divide
  it: a terrace of houses built as one part opens along its whole length.
  A part comes back all at once, a second after it was last in the way.
  Scenery on it is all or nothing: the game draws many copies of one model
  in a single go, so one of them cannot be cut. What sits wholly above the
  cut disappears; a banner that hangs from above the cut to below it stays,
  in the air. And where the library cannot tell which part hides you, it
  cuts everything on screen, as it did before 0.6.0; `/arpg status` says
  which it is doing.
- A creature or an object within 8 yards of your character is drawn wherever
  it stands, also outside the wall. That includes a mailbox by the door.
- Where the game has you indoors but cannot tell which room you stand in
  (`/arpg status` says "this building is not found from inside"), the world
  is drawn as usual and sliced open at the wall height instead, as 0.3.0 did
  everywhere.
- The cut is level and takes everything above it: a creature taller than
  the wall height loses its top. Raise the wall height, or set it all the way
  up for walls to the ceiling.
- A wall inside the room that stands between the camera and your character
  hides what is behind it, up to the wall height.
- The cut needs a Direct3D 9 renderer (`gxApi` d3d9, the default, or d3d9ex;
  DXVK is one). With OpenGL the building is still shown from inside, but
  beams and the floor upstairs stay in the picture.
- With a fog mod that fogs the finished picture by depth, the black behind
  the rooms is fogged like a wall at the far end of the rooms. With
  CoAVolFog that comes out a flat grey in the one picture seen so far.
  `InteriorDensity=0` in `CoAVolFog.ini` should take it away (going by that
  file's own notes; not tried).
- Expected, not yet seen: the name of a creature out in the street may still
  show over the black.

`/arpg cut off` switches it all off, and the "Otherwise, move the camera in
indoors" option then decides what happens under a roof.

## If something is off

- `/arpg status` says whether the library loaded, which parts are working, in
  how many of the client's camera updates the lock was actually applied, and
  what the inside view is doing right now and why: how many rooms it shows,
  whether it found a ceiling and at what height, where it cuts, how many
  frames it has drawn from inside a building, how much it left out for
  standing outside the rooms, how many parts of a building it is cutting
  outdoors, and what the game has under your feet (which part of which kind
  of building, and whether that counts as indoors). If a place shows the
  wrong picture, a screenshot with those lines on it says why.
- `WotLK-ARPG.log`, next to `Wow.exe`, is written at every start. It lists each
  place the library patched, or why it left one alone.
- The library refuses to touch a client it does not recognise: every patch
  site is compared with the expected bytes first, and a mismatch turns that
  one feature off instead of risking a crash.

## Working alongside other mods

- **Other addons** are neither used nor changed. Where this project needs
  something another addon already does (a bigger minimap, a windowed world
  map), it gets its own version, built in.
- **awesome_wotlk**: the library takes over the loader slot and loads
  `AwesomeWotlkLib.dll` itself, so both run.
- **DXVK, CoAVolFog, Lexara HD fonts** and other proxy-DLL mods load through
  other routes and are left alone. Should one of them ever patch a spot this
  library also patches, the library places its own hook in front of theirs
  where it can, and otherwise switches that one feature off and says so in
  the log.
- **CoAVolFog** in particular: none of the places it checks or patches in
  `Wow.exe` is touched (0.4.0 patched the client's fog routine, which
  CoAVolFog patches too; later versions leave it alone). The cut no longer changes
  the depth the game draws with, which that mod's fog and water read. And
  the plane the cut uses is switched off before the sun's glare, floating
  text, names and the mod's own passes are drawn.

## Build and test

```
client/build.sh       # build/WotLK-ARPG.dll and build/mock-client.exe (clang, lld; any OS)
client/test/run.sh    # hook engine test, then the library against a mock client (Linux)
python3 client/test/mutation_test.py            # breaks the cutaway 192 ways; the tests must notice each
python3 client/test/installer_test.py           # the setup script, on a made-up Wow.exe
python3 client/tools/verify_sites.py Wow.exe    # your own Wow.exe against the patch sites
tools/package.sh      # dist/WotLK-ARPG/: what gets installed
```

CI runs all of it but the mutation test on every push, and runs the real DLL
and the setup script on Windows. [docs/DESIGN.md](docs/DESIGN.md) explains
how the pieces work.

## Layout

- `addon/WotLK-ARPG/` - the addon (Lua).
- `client/src/` - the client library (C++, no dependencies beyond kernel32).
- `client/install/` - the setup script and its double-click wrappers.
- `client/test/`, `client/tools/` - tests and the patch-site checker.
- `docs/DESIGN.md` - how it works, and the map of the client's internals.

## Notes

- No game files belong in this repository.
- The client library modifies the running game client. It is meant for a
  server you run yourself; on public servers a modified client can be detected.
- The client research this project leans on was published by the
  [awesome_wotlk](https://github.com/FrostAtom/awesome_wotlk) projects.
