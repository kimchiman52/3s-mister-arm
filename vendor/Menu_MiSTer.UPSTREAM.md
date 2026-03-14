# Menu_MiSTer Vendor Metadata

- Source repository: `https://github.com/MiSTer-devel/Menu_MiSTer.git`
- Pinned commit: `b0a2b9298d7a7a355e4e0a97277d3d4218eb2f55`
- Local vendor path: `vendor/Menu_MiSTer`
- Import date: `2026-03-10`
- Notes:
  - vendored as the Menu-core-derived wrapper seed after hardware validation showed the MemTest-derived seed still produced a black screen on CRT despite correct HPS framebuffer handoff
  - excluded upstream `.git/`, `releases/`, and `*.srf`
  - staged builds rename the project from `menu` to `3SX` and patch the visible `CONF_STR`
