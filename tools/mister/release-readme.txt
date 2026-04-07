3SX — Street Fighter III: 3rd Strike for MiSTer FPGA
=====================================================

A hybrid ARM + FPGA port of 3rd Strike running on the MiSTer DE10-Nano.
Game logic runs on the ARM CPU; the FPGA handles native video output,
audio buffering, and DAC conversion.

This is an experimental release. Expect rough edges.


REQUIREMENTS
------------

You need a copy of SF33RD.AFS, the game data archive. This file is found
inside any PS2 copy of Street Fighter III: 3rd Strike — either the
standalone release or Street Fighter Anniversary Collection. You must
legally own the game.

To extract it: the AFS file is on the PS2 disc at the root or inside a
data directory, typically named SF33RD.AFS (~600 MB).


OVERCLOCK
---------

The ARM CPU defaults to stock 800 MHz. The game generally runs at
60 FPS, but may slow down during heavy situations (animated stage
backgrounds, super art activations, and other effect-heavy scenes).
Overclocking to 1000 or 1200 MHz can help maintain 60 FPS in those
situations.

You can cycle between 800 MHz (stock), 1000 MHz, and 1200 MHz via
the in-game OSD menu (press F12 or the MiSTer menu button). The
setting persists across launches.

NOTE: Not all DE10-Nano boards can run stably at 1200 MHz. If the
game crashes or freezes after overclocking, SSH into your MiSTer and
edit /media/fat/games/3sx/config. Find the arm-clock line and change
it back to stock:

  arm-clock = 800


INSTALLATION
------------

1. Extract this ZIP onto the ROOT of your MiSTer SD card.

   The archive is structured so files land in the right places:

     /media/fat/MiSTer_3SX                  (HPS wrapper)
     /media/fat/_Other/3SX.rbf              (FPGA bitstream)
     /media/fat/games/3sx/bin/3sx            (game binary)
     /media/fat/games/3sx/lib/               (shared libraries)
     /media/fat/games/3sx/scripts/           (launch helpers)

2. Place your SF33RD.AFS file here:

     /media/fat/games/3sx/resources/SF33RD.AFS

3. Edit /media/fat/MiSTer.ini and add the following:

     [3SX]
     main=MiSTer_3SX
     vga_scaler=0
     video_mode=8    ; HDMI users: forces 1080p60 to avoid sync issues

   NOTE: vga_scaler=0 is required. If your global MiSTer settings have
   vga_scaler=1, the per-core override in the [3SX] section is mandatory.
   The native video path will not work with the scaler enabled.

   video_mode=8 sets HDMI to 1920x1080@60 for this core. Without it,
   MiSTer's auto-detection can cause sync issues with 3SX's native video
   signal on some HDMI displays.


RUNNING
-------

1. Boot MiSTer normally.
2. Navigate to the "Other" core folder.
3. Launch "3SX".

If the core immediately exits back to MiSTer, the most common cause is
a missing SF33RD.AFS file.


MORE INFORMATION
----------------

Full documentation, source code, and build instructions:
  https://github.com/kimchiman52/3sx-mister

Upstream project:
  https://github.com/crowded-street/3sx

This project is licensed under the GNU Affero General Public License v3.
