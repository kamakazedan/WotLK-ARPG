# WotLK-ARPG

Diablo-style play for a World of Warcraft 3.3.5a (build 12340) client: a fixed
top-down camera that faces north, hold the left mouse button to move, and WASD
switched off unless you turn it on. Built for a privately run server.

It comes in two parts:

- **The addon** (`Interface/AddOns/WotLK-ARPG`) holds the settings and the
  controls. On its own it gives click-to-move and a parked camera.
- **The client library** (`WotLK-ARPG.dll`, loaded by `Wow.exe`) does what an
  addon is not allowed to: hold-to-move and a camera that cannot be turned,
  zoomed or pushed around by walls.

| Feature | Needs | Status |
|---|---|---|
| One switch for the whole mode, fully reversible | addon | done |
| WASD off unless enabled, toggle works in combat | addon | done |
| No scroll zoom, camera stops following the character | addon | done |
| Hold the left mouse button to keep moving; click to interact, attack, loot | client library | works in game |
| Camera locked facing north at a set angle and distance | client library | fixed in 0.2.1, being proven in game |
| Camera passes through objects instead of zooming in | client library | fixed in 0.2.1, being proven in game |
| Clicks on roofs and treetops move to the ground under the cursor | client library | new in 0.2.1, being proven in game |
| Sound heard from the camera, so left and right match the screen | addon | new in 0.2.1 |
| Camera settings panel | addon | done |
| A skill on the left mouse button (and the right, with WASD on): walk into range, then cast | addon + client library | next |
| Bigger minimap; click it to walk there | CleanMinimap-WotLK + client library | planned |
| World map in a window; click a place to travel there | BetterMaps-WotLK + server pathing | planned |
| Buildings and caves cut away above the character | client library | planned |
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
4. Start the game and type `/arpg`.

`WotLK-ARPG-Uninstall.bat` puts the 20 bytes back. Before running any other
patcher on `Wow.exe`, uninstall first and install again afterwards.

Addon only, no client changes: copy `addon/WotLK-ARPG` into `Interface/AddOns/`.

## Use

| Command | What it does |
|---|---|
| `/arpg` | Turn the mode on or off (`/arpg on`, `/arpg off`) |
| `/arpg wasd` | Lock or unlock WASD movement |
| `/arpg options` | Camera settings (also under Interface > AddOns) |
| `/arpg angle 55` | How steeply the camera looks down, 10 to 89 degrees |
| `/arpg distance 30` | How far the camera is from the character, 5 to 100 yards |
| `/arpg facing 0` | Which way the camera faces; 0 keeps north at the top |
| `/arpg ground` | Roof and treetop clicks go to the ground under the cursor, on or off; `/arpg ground 3` sets from how many yards up |
| `/arpg sound` | Hear the world from the camera or from the character |
| `/arpg camedit` | Free the camera, mouse and scroll wheel for a look around |
| `/arpg status` | Show what is on, and whether the camera lock is really in effect |

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
- **Camera**: stays where the settings put it. Indoors it is allowed to collide
  with walls so the character stays in view; untick that in the options to see
  what it does without. This stopgap goes when buildings are cut away.
- **Scroll wheel and right mouse button**: do nothing, for now.

The mode and its settings are remembered per character: if it was on when you
logged out, it is on, camera and all, when you log back in. Turning it off
puts everything back. The addon never changes your saved key bindings, and it
restores every setting it touched.

Without the client library the addon still works: one click per move,
`/arpg camedit` and `/arpg savecam` to set the camera up by hand.

## If something is off

- `/arpg status` says whether the library loaded, which parts are working, and
  in how many of the client's camera updates the lock was actually applied.
- `WotLK-ARPG.log`, next to `Wow.exe`, is written at every start. It lists each
  place the library patched, or why it left one alone.
- The library refuses to touch a client it does not recognise: every patch
  site is compared with the expected bytes first, and a mismatch turns that
  one feature off instead of risking a crash.

## Working alongside other client mods

- **awesome_wotlk**: the library takes over the loader slot and loads
  `AwesomeWotlkLib.dll` itself, so both run.
- **DXVK, CoAVolFog, Lexara HD fonts** and other proxy-DLL mods load through
  other routes and are left alone. Should one of them ever patch a spot this
  library also patches, the library places its own hook in front of theirs
  where it can, and otherwise switches that one feature off and says so in
  the log.

## Build and test

```
client/build.sh       # build/WotLK-ARPG.dll and build/mock-client.exe (clang, lld; any OS)
client/test/run.sh    # hook engine test, then the library against a mock client (Linux)
python3 client/test/installer_test.py           # the setup script, on a made-up Wow.exe
python3 client/tools/verify_sites.py Wow.exe    # your own Wow.exe against the patch sites
tools/package.sh      # dist/WotLK-ARPG/: what gets installed
```

CI runs all of it on every push, and runs the real DLL and the setup script on
Windows. [docs/DESIGN.md](docs/DESIGN.md) explains how the pieces work.

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
