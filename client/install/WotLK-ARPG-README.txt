WotLK-ARPG
==========

Diablo-style camera and controls for a World of Warcraft 3.3.5a (12340) client.
Project and full instructions: https://github.com/kamakazedan/WotLK-ARPG

What is here
------------
  WotLK-ARPG.dll              the client library
  WotLK-ARPG-Install.bat      makes Wow.exe load the library
  WotLK-ARPG-Uninstall.bat    undoes that
  WotLK-ARPG-Setup.ps1        the script both of them run
  Interface\AddOns\WotLK-ARPG the addon

Install
-------
1. Close the game.
2. Put everything from this package into the game folder, next to Wow.exe.
   The Interface folder merges with the one that is already there.
3. Double-click WotLK-ARPG-Install.bat.
   It first copies Wow.exe to Wow.exe.before-WotLK-ARPG, then changes the name
   of the DLL that the loader inside Wow.exe asks for. Nothing else is touched.
   Your Wow.exe must already have that loader: St0ny's AIO patcher adds it
   (patch "Load AwesomeWotlkLib.dll at startup"), as does awesome_wotlk's own
   patcher. The script tells you if it is missing.
4. Start the game, log in, type /arpg.

In game
-------
  /arpg            turn the mode on or off
  /arpg options    camera angle, distance and facing
  /arpg status     what is on, and whether the library is working
  /arpg help       everything else

Hold the left mouse button to move. Click a creature, object or corpse to walk
up and interact, attack or loot.

If something is off
-------------------
WotLK-ARPG.log, written next to Wow.exe at every start, lists what the library
did. /arpg status shows the same from inside the game.

Uninstall
---------
Double-click WotLK-ARPG-Uninstall.bat, then delete the files listed above.
Run it before using any other patcher on Wow.exe, and install again afterwards.
