3SX MiSTer install
==================

This ZIP is FAT-rooted. Extract it directly onto the root of your MiSTer SD card so it creates:

- /media/fat/MiSTer_3SX
- /media/fat/_Other/3SX.rbf
- /media/fat/games/3sx/...

Required game data
------------------

This release ZIP does not include SF33RD.AFS.

After extracting the ZIP, add your legally obtained archive here:

/media/fat/games/3sx/resources/SF33RD.AFS

MiSTer.ini
----------

Add this section to /media/fat/MiSTer.ini if it is not already present:

[3SX]
main=MiSTer_3SX

Optional CRT-friendly overrides:

[3SX]
main=MiSTer_3SX
video_mode=384,240,60
vga_scaler=1

Launch
------

Select the 3SX core from MiSTer after the files are in place.
