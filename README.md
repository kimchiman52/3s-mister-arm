# 3SX

A port of the greatest fighting game of all time for modern platforms.

Requires an official copy of *Street Fighter III: 3rd Strike* or *Street Fighter Anniversary Collection* for PlayStation 2 to play.

Based on a [decompilation](https://github.com/crowded-street/sfiii-decomp) of the PlayStation 2 port.

## Experimental MiSTer Branch

This repository is a highly experimental MiSTer-focused branch of the upstream
[crowded-street/3sx](https://github.com/crowded-street/3sx) project.

Important:

- this branch is not the canonical upstream project
- development here is fast-moving and much of the recent implementation work has been AI-assisted
- expect rough edges, regressions, and breaking changes between revisions

## MiSTer Install

If you are installing from a player release ZIP, extract it directly onto the root of your MiSTer
SD card. The archive is FAT-rooted and should place files here:

- `/media/fat/MiSTer_3SX`
- `/media/fat/_Other/3SX.rbf`
- `/media/fat/games/3sx/`

The release ZIP does not include the required game archive. After extracting the ZIP, place your
legally obtained `SF33RD.AFS` here:

- `/media/fat/games/3sx/resources/SF33RD.AFS`

Then add this section to `/media/fat/MiSTer.ini` if it is not already present:

```ini
[3SX]
main=MiSTer_3SX
```

```ini
[3SX]
main=MiSTer_3SX
```

If your global MiSTer settings have `vga_scaler=1`, add `vga_scaler=0`
to the `[3SX]` section to override it.

## Running On MiSTer

After the files are installed and `SF33RD.AFS` is in place:

1. Boot MiSTer normally.
2. Open the `Other` core folder.
3. Launch `3SX`.

If the game immediately returns to MiSTer, the most common missing piece is:

- `/media/fat/games/3sx/resources/SF33RD.AFS`

## Resources

Find instructions on [how to build](docs/building.md) the project and other useful resources in the [docs](docs) folder.

## Community

Join `Crowded Street` server on Discord to discuss the project, report bugs or share your ideas!

[![Join the Discord](https://dcbadge.limes.pink/api/server/https://discord.gg/wqs6BqYr8C)](https://discord.gg/wqs6BqYr8C)

## Acknowledgments

This project uses:
- [GekkoNet](https://github.com/HeatXD/GekkoNet) for P2P rollback netcode
- [FFmpeg](https://ffmpeg.org) for ADX playback
- [SDL3](https://github.com/libsdl-org/SDL) for window management, input handling, sound output and rendering
- SDL_net for P2P connections
- [libcdio / libiso9660](https://github.com/libcdio/libcdio) for .iso file reading
- [zlib](https://zlib.net) for file decompression
- [argparse](https://github.com/cofyc/argparse) for parsing CLI arguments
- [minizip-ng](https://github.com/zlib-ng/minizip-ng) for unzipping
- [TF-PSA-Crypto](https://github.com/Mbed-TLS/TF-PSA-Crypto) for checksum calculation
- [stb](https://github.com/nothings/stb) for data structures
