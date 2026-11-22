# WotLK-ARPG

Diablo-style play for a World of Warcraft 3.3.5a (build 12340) client: a fixed
top-down camera, click or hold the left mouse button to move, and WASD switched
off unless you turn it on. Built for a privately run server.

**Status: early.** The addon works on a stock client today. The client library
that adds hold-to-move, the locked camera and the building cutaway comes next.

| Feature | Needs | Status |
|---|---|---|
| One switch for the whole mode, fully reversible | addon | done (v0.1) |
| Left-click moves and interacts, right-click freed for a skill | addon | done (v0.1) |
| No scroll zoom, camera stops following the character | addon | done (v0.1) |
| WASD off unless enabled, toggle works in combat | addon | done (v0.1) |
| Hold left-click to keep moving | client library | next |
| Camera locked facing north at a set height and angle | client library | next |
| Camera passes through objects instead of zooming in | client library | next |
| Buildings and caves cut away above the character | client library | planned |
| Skills aim at the cursor, character turns to face the target | client library | planned |
| Skill bar, health and resource orbs, overlay map | addon | planned |
| Casting while moving, dodge, pathing around obstacles | server + Spell.dbc | planned |

## Install the addon

Copy the folder `addon/WotLK-ARPG` into your client's `Interface/AddOns/`
folder, so that you end up with `Interface/AddOns/WotLK-ARPG/WotLK-ARPG.toc`.

## Use

| Command | What it does |
|---|---|
| `/arpg` | Turn the mode on or off (`/arpg on`, `/arpg off`) |
| `/arpg wasd` | Lock or unlock WASD movement |
| `/arpg camedit` | Free the mouse and scroll wheel so you can set up the camera |
| `/arpg savecam` | Save the current camera as the ARPG camera |
| `/arpg cam` | Snap back to the saved camera |
| `/arpg status` | Show the current state |

Keys for the mode and for the WASD toggle are under **Key Bindings > WotLK-ARPG**.
Bind the WASD toggle there: it is the one that works in combat.

First run: `/arpg`, then `/arpg camedit`, zoom out and tilt the camera to the
height and angle you want, then `/arpg savecam`.

Turning the mode off puts everything back. The addon never changes your saved
key bindings, and it restores every setting it touched.

## Layout

- `addon/WotLK-ARPG/` - the addon (Lua).
- `docs/DESIGN.md` - how the pieces fit, and what each one can and cannot do.
- `client/` - the client library (C++). Not here yet.

## Notes

- No game files belong in this repository.
- The client library modifies the game client. It is meant for a server you
  run yourself; on public servers a modified client can be detected.
- The client research this project leans on was published by the
  [awesome_wotlk](https://github.com/FrostAtom/awesome_wotlk) projects.
