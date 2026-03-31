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

[3SX]
main=MiSTer_3SX

If your global MiSTer settings have vga_scaler=1, add vga_scaler=0
to the [3SX] section to override it.

Launch
------

Select the 3SX core from MiSTer after the files are in place.
