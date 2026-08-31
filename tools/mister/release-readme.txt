3s-mister-arm — Street Fighter III: 3rd Strike for MiSTer FPGA
==============================================================

A hybrid ARM + FPGA port of 3rd Strike running on the MiSTer DE10-Nano.
Game logic runs on the ARM CPU; the FPGA handles native video output,
audio buffering, and DAC conversion.

This is an experimental release. Expect rough edges.


UPGRADING FROM A PRE-RENAME BUILD
---------------------------------

This project was formerly "3SX MiSTer" and has been renamed to
3s-mister-arm. The INI section, wrapper executable, and game data
directory all changed. After extracting the new release:

1. In /media/fat/MiSTer.ini, rename your [3SX] section to [3S-ARM]
   and change main=MiSTer_3SX to main=MiSTer_3S-ARM.

2. Move your game data from /media/fat/games/3sx/ to
   /media/fat/games/3s-arm/ (most importantly resources/SF33RD.AFS).
   Config, saves, and logs all live under this directory too.

3. You can delete the old /media/fat/MiSTer_3SX wrapper and
   /media/fat/_Other/3SX.rbf bitstream — the new release ships as
   MiSTer_3S-ARM and 3S-ARM_YYYYMMDD.rbf (date-suffixed per the
   MiSTer cores convention).


REQUIREMENTS
------------

You need a copy of SF33RD.AFS, the game data archive. This file is found
inside any PS2 copy of Street Fighter III: 3rd Strike — either the
standalone release or Street Fighter Anniversary Collection. You must
legally own the game.

To extract it: the AFS file is on the PS2 disc at the root or inside a
data directory, typically named SF33RD.AFS (~600 MB).


INSTALLATION
------------

1. Extract this ZIP onto the ROOT of your MiSTer SD card.

   The archive is structured so files land in the right places:

     /media/fat/MiSTer_3S-ARM                  (HPS wrapper)
     /media/fat/_Other/3S-ARM_YYYYMMDD.rbf     (FPGA bitstream — dated)
     /media/fat/games/3s-arm/bin/3s-arm        (game binary)
     /media/fat/games/3s-arm/lib/              (shared libraries)
     /media/fat/games/3s-arm/scripts/          (launch helpers)

   ** FTP users: ** If transferring files via FileZilla or another
   FTP client, set the transfer type to Binary (not Auto or ASCII).
   The default mode corrupts extensionless binaries like MiSTer_3S-ARM
   and 3s-arm, causing the core to crash on launch.

2. Place your SF33RD.AFS file here:

     /media/fat/games/3s-arm/resources/SF33RD.AFS

3. Edit /media/fat/MiSTer.ini and add the following:

     [3S-ARM]
     main=MiSTer_3S-ARM

4. (Optional) Arcade balance. If you have a CPS3 arcade core installed,
   3S-ARM reads that core's own romset and switches to verified arcade
   (CPS3) balance automatically at boot. No copy of the ROM is shipped
   with this release and none needs to be placed under games/3s-arm/.
   The romset is read from whichever of these your core already uses:

     /media/fat/games/mame/sfiii3nr1.zip
     /media/fat/games/mame/sfiii3.zip

   With neither present the game runs PS2 balance, which is the normal
   default; the choice and its reason are written to
   /media/fat/games/3s-arm/balance.status on every launch.


CRT TROUBLESHOOTING
-------------------

The native video path outputs at NTSC standard horizontal frequency
(15,734 Hz). Most 15 kHz CRTs and arcade monitors should sync directly
with vga_scaler=0 (the default).

If you still cannot get a picture, try using the MiSTer scaler with
this custom video mode:

     [3S-ARM]
     main=MiSTer_3S-ARM
     vga_scaler=1
     video_mode=384,22,38,51,224,16,3,21,7788

Then open the OSD (F12) and set "Aspect Ratio" to "Full". This routes
through the MiSTer scaler at native 384x224 NTSC timing without pixel
resampling. If your CRT expects composite sync, also add
composite_sync=1 to the [3S-ARM] section. Note: vga_scaler=1 disables
S-Video color output.



RUNNING
-------

1. Boot MiSTer normally.
2. Navigate to the "Other" core folder.
3. Launch "3S-ARM".

If the core immediately exits back to MiSTer, the most common cause is
a missing SF33RD.AFS file.


TRAINING MODE
-------------

Press SELECT during training to reset both characters back to their
round-start positions. There is no screen wipe and the music keeps
playing, so it is much faster than restarting the round from the menu.
A short confirmation sound plays when the reset fires.

Holding DOWN while pressing SELECT does the same thing. Holding UP,
LEFT or RIGHT does nothing yet — those will pick a starting arrangement
(side swap, and cornering either player) in a later release.

START + SELECT is unchanged and still soft-resets the core.

The reset is disabled while a recording or replay is running, so a
recorded dummy sequence cannot be knocked out of sync.


OVERCLOCK
---------

The ARM CPU defaults to stock 800 MHz. The game generally runs at
60 FPS, but may slow down during heavy situations (animated stage
backgrounds, super art activations, and other effect-heavy scenes).
Overclocking to 1000 or 1200 MHz can help maintain 60 FPS in those
situations.

You can cycle between 800 MHz (stock), 1000 MHz, and 1200 MHz via
the in-game OSD menu (press F12 or the MiSTer menu button). The new
clock speed takes effect on the next game restart (shown with a *
in the OSD until applied). The setting persists across launches.

NOTE: Not all DE10-Nano boards can run stably at 1200 MHz. If the
game crashes or freezes after overclocking, SSH into your MiSTer and
edit /media/fat/games/3s-arm/config. Find the arm-clock line and change
it back to stock:

  arm-clock = 800


MORE INFORMATION
----------------

Full documentation, source code, and build instructions:
  https://github.com/kimchiman52/3s-mister-arm

Upstream project:
  https://github.com/crowded-street/3sx

This project is licensed under the GNU Affero General Public License v3.
