# Config

3S-ARM supports a config file which allows you to change several useful options.

Config location:
- **Windows**: `C:\Users\<username>\AppData\Roaming\CrowdedStreet\3S-ARM\config`
- **Linux**: `~/.local/share/CrowdedStreet/3S-ARM/config`
- **macOS**: `~/Library/Application Support/CrowdedStreet/3S-ARM/config`

## Options

### `fullscreen`

Whether the game should start in fullscreen mode.

### `window-width` / `window-height`

Window dimensions to use when `fullscreen` is set to `false`.

### `scale-mode`

The way the internal 384x224 buffer is scaled.

Possible values:
- `native`: No scaling. Keeps the internal `384x224` image size and centers it. On MiSTer's 384-native analog TV path, this preserves the game's native width in the framebuffer.
- `nearest`
- `linear`
- `soft-linear`: Produces an image with a balance of sharpness and sizing consistency
- `integer`: Produces a pixel-perfect image, but requires a 4K display (⚠️ WARNING: the image is gonna be cropped if your display resolution is smaller than 2688x2016)
- `square-pixels`: Integer (whole-number) scaling with square pixels. Use this if you play on a CRT

### `scanlines`

Defines the strength of the scanline filter (from `0` to `100`). `0` means the filter is disabled.

### `draw-players-above-hud`

Allow characters to render in front of the top HUD similar to Street Fighter IV. May introduce visual abnormalities on certain stages.

With this setting on, these stage decorations are disabled to prevent overlapping with the HUD:
- Chun-Li's stage: bamboo stick on the right
- Makoto's stage: tree on the right
- Yang's stage: rain overlay

### `balance`

Arcade (CPS3) vs PS2 balance **auto-selects at boot** — there is no OSD
toggle. Values:

- `auto` (default): arcade balance is used when a CPS3 ROM source passes
  content verification AND the full 20-character adaptation succeeds;
  anything else falls back to PS2 balance with the reason logged and written
  to `<pref>/balance.status` (line 1: `Arcade (CPS3)` / `PS2`; line 2: the
  reason). Adaptation is all-or-nothing: a single character failing means the
  whole session is PS2, never a per-character mix.
- `ps2`: force PS2 balance even with a valid ROM (config-file-only knob for
  players who own the ROM but prefer PS2 balance).

ROM discovery tries, in order: `resources/sfiii3nr1.zip`, the
`THIRDSARM_CPS3_ZIP` env override, then `/media/fat/games/mame/sfiii3.zip`
(the merged set update_all ships for the jtcps3 core). Entries are matched by
content hash (stored-CRC32 pre-filter + SHA-256 confirmation), so flat,
merged, and subdirectory-variant packagings all work.

Notes:
- Replaces the removed `arcade-balance` bool toggle; stale `arcade-balance`
  lines in existing configs are ignored.
- A hand-added `balance = ps2` line SURVIVES the MiSTer wrapper's config
  rewrites. Every `write_runtime_*_default()` in
  `vendor/Main_MiSTer/thirdsarm_wrapper.cpp` is a line-preserving
  copy-through: it rewrites only the single line whose trimmed key
  `strcasecmp`-matches its own target and emits every other line verbatim
  via `fputs(line, out)`. The 15 targeted keys (`scale-mode`, `arm-clock`,
  `game-mode`, `hold-to-pause`, `arcade-balance`, `bgm-type`,
  `aspect-ratio`, `h-position`, `v-position-v2`, `v-position`,
  `vertical-crop`, `crop-offset`, `scale`, `h-size`, `show-fps`) do not
  include `balance`, and no writer regenerates the file from a template.
- Netplay arms only in verified-arcade state and the MIST handshake carries a
  digest of the adapted data, so peers always simulate identical balance.
- The test runner (`--test-enable`, used by the frame-data suite) pins PS2
  balance regardless of ROM presence so corpus expectations stay hermetic.

### `bgm-type`

Selects between the PS2 arranged soundtrack (`arranged`, default) and the
original arcade soundtrack (`original`). Mirrors the in-game Sound Options
menu's BGM Type setting (`sys_w.bgm_type`, `BGM_ARRANGED`/`BGM_ORIGINAL` in
`structs.h`).

On MiSTer this is exposed as the **BGM Type** option in the OSD menu (status
bit `[14]`). The wrapper writes the toggle into this config key, so it
persists across launches; like Arcade Balance it applies on the **next game
launch** (use OSD → Restart), because the boot-time override
(`BgmType_ApplyBootOverride()`) only runs once, right after the settings save
file finishes loading.

Two-way sync: changing BGM Type from the in-game Sound Options menu also
writes this config key (`BgmType_PersistToConfig()`), so the OSD reflects an
in-game change at the next launch too, and the two settings never drift
apart from each other.

### `software-frame-mode`

Controls whether gameplay uses the 3S-ARM-owned software frame path or the legacy SDL-owned gameplay frame path.

Defaults:
- MiSTer builds: `on`
- Non-MiSTer builds: `off`

Possible values:
- `off`: Keep the existing SDL-owned gameplay frame path
- `on`: Keep the `384x224` gameplay frame in 3S-ARM-owned software memory. On MiSTer, eligible frames present directly through fbdev to avoid SDL readback; when composition or screenshots still need SDL, the frame uploads back to `cps3_canvas`.

### `super-effect-quality`

Controls MiSTer-only rendering optimization during super art activation.

**Dev-only:** the OSD knob has been removed. Flip via `config.ini` for
release builds; the runtime read path is preserved.

Defaults:
- MiSTer builds: `cached-bg`
- Non-MiSTer builds: `full`

Possible values:
- `full`: No reduction — render every frame fully
- `cached-bg`: Cache the rendered background surface on the first super art activation frame, then restore it via fast blit on subsequent frames while rendering characters/effects/HUD fresh at 60fps

Notes:
- This setting is only active on MiSTer builds. On non-MiSTer builds the config key is parsed but behaves like `full`.
- Detection uses `sa_stop_check()` (cinematic freeze signal) which fires for all characters and all super arts. A grace period of ~15 frames after the signal drops covers the zoom-out transition.

### `show-fps`

Whether to draw a small FPS readout while the game is running.

Defaults:
- `false`

Notes:
- On MiSTer fbdev output, the overlay is drawn at the bottom-center of the active picture area so it stays away from overscan-prone corners.
- The overlay is opt-in and uses a lightweight cached label update path instead of perf capture telemetry.

### `video-driver-order`

Comma-separated SDL video backend preference list passed via `SDL_HINT_VIDEO_DRIVER` before SDL init.

Example:
- `kmsdrm,offscreen,dummy`

### `render-driver-order`

Comma-separated SDL renderer backend preference list passed via `SDL_HINT_RENDER_DRIVER` before renderer creation.

Example:
- `software`

### Direct-P2P (`netplay-direct-p2p-*`)

Runtime knobs for the STUN/UPnP-based direct-peer connect flow
(docs/plan-stun-direct-p2p.md). Users coordinate the peer code
out-of-band (text message, voice) and paste it into the wrapper OSD.

`netplay-direct-p2p-host-port` (int, default `0`)
- UDP port the host wants to bind for Direct-P2P. `0` means
  OS-assigned ephemeral. Advisory — if the port is already bound the
  OS falls back to ephemeral; the public port reported to the peer
  is whatever the NAT maps it to, not what you requested.

`netplay-direct-p2p-disable-upnp` (bool, default `false`)
- Skip the UPnP IGD first-try path and go straight to STUN hole
  punch. Set `true` if your router misbehaves on UPnP and fails
  slowly (some routers reply to `UPNP_Discover` but then hang on
  `AddPortMapping`). STUN fallback always runs regardless.

`netplay-direct-p2p-last-peer-code` (string, no default)
- Populated at runtime on a successful Join to enable quick-rejoin
  on a "match lost" reconnect flow. Not persisted across runs in
  this phase (Config_Save is a stub until a future cleanup).

`netplay-direct-p2p-handoff-path` (string, default
`/tmp/3s-arm-netplay.handoff`)
- File the wrapper writes Host/Join intent into before exec'ing the
  game binary. tmpfs path; contents are readable only by root (mode
  `0600`) and consumed-and-unlinked by the game on first read.

`netplay-direct-p2p-stun-timeout-ms` (int, default `4000`)
- Overall wall-clock budget for STUN discovery. Since S2
  (docs/plan-netplay-connection.md §4) all four servers are probed in
  PARALLEL from the one socket with per-server retransmits at
  0/500/1500 ms, DNS resolves concurrently on a side thread, and a
  numeric-IP fallback list covers a dead resolver — the first
  parseable Binding Response wins. Clamped to 1000–15000 ms: the floor
  because below one RTO the retransmit ladder is meaningless, the
  ceiling because `Stun_Discover` is not cancellable mid-run and
  cancelling Host/Join mid-discovery blocks the game thread for up to
  this budget. (Pre-S2 this key was read by nothing; the serial
  implementation had its own hard-coded ~2 s/server budget.)

`netplay-direct-p2p-disable-bilateral` (bool, default `false`)
- If true, disables the bilateral hole-punch fallback. Direct-P2P will
  terminate with "Cannot reach peer. Possible Symmetric NAT." on punch
  timeout, matching behavior before the bilateral feature landed.

`netplay-direct-p2p-signal-url` (string, default
`udp://46.62.244.55:3478`)
- Rendezvous server URL for bilateral hole-punch endpoint exchange.
  Only used when the direct punch fails; never contacted on LAN sessions.
  UDP scheme only.

`netplay-direct-p2p-signal-budget-ms` (int, default `8000`)
- Maximum wall-clock time spent waiting for the rendezvous server to
  pair us with our peer before giving up and reporting bilateral failure.
- Since S1 (docs/plan-netplay-connection.md) this bounds the JOINER's
  signaling phase only; the host's re-REGISTER loop is persistent and
  paced by `netplay-direct-p2p-register-interval-ms` instead.

`netplay-direct-p2p-bilateral-punch-ms` (int, default `5000`)
- Length of the bilateral Stun_HolePunch window, in milliseconds.
  Longer than the initial direct punch window to accommodate post-
  signaling clock skew between peers. Raised from 3000 in S2
  (docs/plan-netplay-connection.md §4): the two sides' punch windows
  start skewed by DELIVER-arrival timing, and both loops tolerate
  stray datagrams, so extra overlap only costs failure-case wait.

`netplay-direct-p2p-register-interval-ms` (int, default `5000`)
- Host liveness (S1): cadence of the host's persistent rendezvous
  re-REGISTER loop while the room code is displayed. Keeps the
  rendezvous session and the host->server NAT mapping alive for as
  long as the host is advertising. Floor-clamped to 1000 ms (the
  server rate-limits at 10 packets/s/IP). The whole loop is disabled
  by `netplay-direct-p2p-disable-bilateral`.

`netplay-direct-p2p-stun-keepalive-ms` (int, default `15000`)
- Host liveness (S1): cadence of the STUN rebind keepalive while
  waiting for a joiner. Refreshes the advertised NAT mapping and
  detects public-endpoint drift; a drift must be confirmed by two
  consecutive keepalives before the room code is re-encoded and
  re-displayed with an explicit "Network changed" status (a NAT that
  rebinds on every keepalive therefore never churns the code). The
  default sits below common NAT UDP idle timeouts (~30 s) so the
  mapping is held rather than lost. Set to `0` (or negative) to
  disable; values below 5000 ms are clamped up.

### Netplay tuning

`netplay-input-prediction-window` (int, default `8`)
- GekkoNet rollback prediction window — max frames the local sim is
  allowed to predict ahead of confirmed inputs and, on mispredict, the
  max rollback depth. Lower values cut worst-case resim CPU (linear in
  the window) at the cost of tolerating less input lag before the
  client falls back to stutter. Upstream GekkoNet default is 10; we
  use 8 by default tuned for stock-clock MiSTer (800 MHz, near-zero
  headroom above 60 fps).

`netplay-diag-enable` (bool, default `true`)
- Master gate for session-tagged diagnostics: per-session UUID, UTC
  timestamp, jitter, kbps_tx/rx heartbeat extras, and the
  post-disconnect desync state dump. Recording cost is paid in the
  production codepath regardless; this knob is an emergency mute for
  serial-console noise. Default `true` so a friend running a build for
  a shared session contributes useful logs without having to opt in.
- See [docs/netplay-diagnostics.md](netplay-diagnostics.md) for the
  full set of instrumentation, log formats, and dump file locations.

`netplay-sparse-effect-save-enabled` (bool, default `true`)
- Enable the Option A sparse effect-pool GameState save path. When on
  (the default), each rollback save serializes only the active slots
  of the effect work pool (frw[128]) and reconstructs canonical empty
  slots on load. Typical per-frame state size drops from ~247 KB to
  60–120 KB depending on activity, shrinking GekkoNet's per-save
  checksum/transmission/load memcpy proportionally. Cross-peer desync
  detection is unaffected (the focused checksum never walked frw[]).
  Set `false` to force the legacy full-state save path for A/B parity
  testing without rebuilding. Active-slot counts above the 70-slot
  ceiling are auto-handled: the save falls back to a full-state pack
  for that frame and emits a deduped warning. Empirical peak from the
  on-device Pk\<N\> peak watcher across full-roster super-art-heavy
  sessions was 57; the 70 ceiling is a deliberate ~1.23x margin.
