# Miyoo Mini Plus Runbook

End-to-end build → package → deploy → run loop for the OnionOS port.

## Device

- Host: `192.168.1.190` (dropbear on port 22)
- User: `root`, password: `onion`
- Device-side root: `/mnt/SDCARD/`
- App dir on device: `/mnt/SDCARD/Roms/PORTS/Games/3s-arm/`
- OnionOS shortcut: `/mnt/SDCARD/Roms/PORTS/Shortcuts/Action/3s-arm.port`

SSH only listens while WiFi *and* the OnionOS SSH toggle are both on.
Flip both before any deploy. When WiFi is off, dropbear is killed
automatically — there is no "SSH-only no-WiFi" mode.

## Build

```sh
tools/miyoo/build-game.sh
```

Cross-compiles in the `3s-miyoo-arm-build` Docker container against the
union-miyoomini-toolchain Buildroot sysroot using `clang-13`. Outputs:

- `build/miyoo-install/` — cmake install prefix
- `build/miyoo-package/` — SD-card-rooted layout (`Roms/PORTS/...`)
  ready to mirror onto the device

`tools/miyoo/build-runtime-package.sh` re-runs only the package step
when you've already built and just want to refresh the package tree.
`tools/miyoo/build-release.sh` adds the install README + zip for an
end-user release.

## Deploy

```sh
# Full package over SSH (default — host=192.168.1.190, user=root,
# password from $MIYOO_PASSWORD or 'onion').
tools/miyoo/deploy.sh

# Inspect the file list without mutating the device.
tools/miyoo/deploy.sh --dry-run

# Sync only the app dir (skip shortcut + Imgs placeholder).
tools/miyoo/deploy.sh --app-only

# Push only the .port shortcut (after editing it).
tools/miyoo/deploy.sh --shortcut-only
```

The deploy script never passes `--delete`. SF33RD.AFS, savestates,
launch.log, keymap edits, other apps under `Roms/`, etc. are preserved
because they're not part of the package source tree. Dropbear runs as
root, so write-permissions on `/mnt/SDCARD/` are always sufficient.

If you need to push to a path other than `/mnt/SDCARD/` (rare — only
needed for testing on a non-default Onion install), set both:

```sh
MIYOO_UNSAFE_ALLOW_ANY_REMOTE_ROOT=1 \
MIYOO_UNSAFE_CONFIRM_REMOTE_ROOT=/some/other/path \
  tools/miyoo/deploy.sh --remote-root /some/other/path
```

The two-step override exists to guard against accidentally pointing
the rsync at, say, `/` because of a typo.

### SD-card fallback

Dropbear is convenience, not load-bearing. If WiFi is off or the SSH
toggle is off, you can always pull the SD, mount it on the host, and
mirror the package tree onto the SD root:

```sh
rsync -a --exclude='._*' build/miyoo-package/ /Volumes/<SD>/
```

Same `--exclude='._*'` rule — it skips macOS AppleDouble metadata
files. Same no-`--delete` rule — the SD root is shared.

## Run + log retrieval

The on-SD app dir captures stdout/stderr from the .port script and the
binary into `launch.log`:

```sh
sshpass -p onion ssh -o StrictHostKeyChecking=no \
  root@192.168.1.190 \
  'tail -200 /mnt/SDCARD/Roms/PORTS/Games/3s-arm/launch.log'
```

For an SSH-driven launch outside the OnionOS Ports menu (skips the
`launch_standalone.sh` wrapper, useful for forensic exec):

```sh
sshpass -p onion ssh -o StrictHostKeyChecking=no \
  root@192.168.1.190 \
  'cd /mnt/SDCARD/Roms/PORTS/Games/3s-arm && ./launch.sh'
```

`launch.sh` re-applies the same env the .port script sets (audio
backend, evdev devices, LD_LIBRARY_PATH) minus the OnionOS performance
governor pin.

## Asset placement

The first deploy lands a placeholder:

```
/mnt/SDCARD/Roms/PORTS/Games/3s-arm/resources/PUT_SF33RD_AFS_HERE.txt
```

Drop the user's legally-owned `SF33RD.AFS` next to it (same dir, name
`SF33RD.AFS`). `launch_standalone.sh` checks the file exists before
running the binary; if missing, OnionOS shows a popup pointing at the
expected path.

The deploy script never overwrites or removes `SF33RD.AFS` — the
package source tree doesn't contain one, and `--delete` is never
passed.

## Dropbear lifecycle quick-ref

Per `/mnt/SDCARD/.tmp_update/script/network/update_networking.sh`:

- WiFi OFF → dropbear is killed.
- WiFi ON + SSH toggle OFF → dropbear is not started.
- WiFi ON + SSH toggle ON, `authsshState=ON` → `dropbear -R`
  (real auth required, password=`onion`).
- WiFi ON + SSH toggle ON, `authsshState=OFF` → `dropbear -R -B`
  (no-auth mode — useful for first-touch deploy when the password
  isn't known yet; accepts any password for any user).
