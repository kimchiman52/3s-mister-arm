3s-miyoo-arm — Street Fighter III: 3rd Strike for Miyoo Mini Plus
==================================================================

A pure-userspace ARM port of 3rd Strike running on the Miyoo Mini Plus
under OnionOS. Renders to the panel via SigmaStar MI_GFX, drives audio
through the SigmaStar MI_AO HAL directly (bypassing /dev/dsp).

This is an experimental release. Expect rough edges.


REQUIREMENTS
------------

A Miyoo Mini Plus running OnionOS. Stock firmware is not supported.

You also need a copy of SF33RD.AFS, the game data archive. This file is
found inside any PS2 copy of Street Fighter III: 3rd Strike — either
the standalone release or Street Fighter Anniversary Collection. You
must legally own the game.

To extract it: the AFS file is on the PS2 disc at the root or inside a
data directory, typically named SF33RD.AFS (~600 MB).


INSTALLATION
------------

1. Extract this ZIP onto the ROOT of your Miyoo SD card.

   The archive is structured so files land in the right places:

     /mnt/SDCARD/Roms/PORTS/Games/3s-arm/3s-arm                (binary)
     /mnt/SDCARD/Roms/PORTS/Games/3s-arm/launch.sh             (launcher)
     /mnt/SDCARD/Roms/PORTS/Games/3s-arm/keymap                (default mapping)
     /mnt/SDCARD/Roms/PORTS/Games/3s-arm/lib/libSDL3.so*       (bundled SDL3)
     /mnt/SDCARD/Roms/PORTS/Shortcuts/Action/3s-arm.port       (OnionOS entry)
     /mnt/SDCARD/Roms/PORTS/Imgs/PUT_3S_ARM_PNG_HERE.txt       (icon placeholder)

   ** macOS users: ** macOS adds AppleDouble metadata files (._<name>) to
   exFAT volumes. These are harmless on the device but clutter the SD
   card. To skip them, use rsync from a terminal instead of Finder:

     rsync -a --exclude='._*' /path/to/extracted/ /Volumes/<SD>/

2. Place your SF33RD.AFS file here:

     /mnt/SDCARD/Roms/PORTS/Games/3s-arm/resources/SF33RD.AFS

3. Optional: drop a 250×376 PNG icon at
   /mnt/SDCARD/Roms/PORTS/Imgs/3s-arm.png to give the OnionOS shortcut
   a custom thumbnail. The placeholder file documents the path.


RUNNING
-------

1. Boot the Miyoo Mini Plus normally (OnionOS main menu).
2. Open the GAMES tab → PORTS → Action.
3. Launch "3rd Strike".

If the launcher exits immediately back to the menu, the most common
cause is a missing SF33RD.AFS file at the path above. Check the on-SD
log at /mnt/SDCARD/Roms/PORTS/Games/3s-arm/launch.log for details.


CONTROLS
--------

Default mapping (can be edited via the on-SD `keymap` file):

  D-pad       — movement
  A           — Medium Kick
  B           — Light Kick
  X           — Medium Punch
  Y           — Light Punch
  L1          — Heavy Punch
  R1          — Heavy Kick
  L2          — 3-Punch macro
  R2          — 3-Kick macro
  Start       — pause / menu confirm
  Select      — back / menu cancel


PERFORMANCE
-----------

The game targets 60 FPS at the Miyoo's stock 1.2 GHz clock. Vsync is
enabled; the SigmaStar MI_GFX hardware blit handles the canvas →
framebuffer copy off-CPU. PerformanceMode=1 in the OnionOS launcher
pins the CPU governor so you don't pay scaling overhead.

If you observe slowdown in heavy stages, you can enable the on-screen
FPS counter via the in-game menu (when implemented) or by editing
config to set show-fps-overlay = 1.


TROUBLESHOOTING
---------------

* No audio: the launcher kills OnionOS' audioserver to claim exclusive
  access to MI_AO. If audio cuts out across multiple games, reboot the
  device — audioserver respawns automatically after the port exits.

* No buttons / device hangs at title: SDL3 input enumeration runs
  through evdev. The launcher sets SDL_EVDEV_DEVICES=2:/dev/input/event0
  which matches OnionOS' keymon. If you've remapped keymon, edit
  launch.sh accordingly.

* Slow / garbled audio: this should not happen with the direct MI_AO
  backend. If it does, capture launch.log and report.


MORE INFORMATION
----------------

Full documentation, source code, and build instructions:
  https://github.com/kimchiman52/3s-mister-arm

Upstream project:
  https://github.com/crowded-street/3sx

This project is licensed under the GNU Affero General Public License v3.
