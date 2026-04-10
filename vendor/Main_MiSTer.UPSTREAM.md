# Main_MiSTer Vendor Metadata

- Source repository: `https://github.com/MiSTer-devel/Main_MiSTer.git`
- Pinned commit: `3380931329b8acb442bd3d35a24d89f88641b7cf`
- Import intent: phase 1 `MiSTer_3S-ARM` HPS wrapper foundation
- Local vendor path: `vendor/Main_MiSTer`

This snapshot is intentionally represented in git as a small tracked overlay, not as a full vendored
copy. The full pinned upstream tree is fetched on demand by `tools/mister-wrapper/build-hps.sh`,
then the local 3S-ARM-specific overlay files are applied on top.

The checked-in overlay subset lives under `vendor/Main_MiSTer`. The current overlay list is
`tools/mister-wrapper/main-mister-overlay.files` and contains only the files that intentionally
diverge from upstream for the `MiSTer_3S-ARM` wrapper build plus the Docker fallback context.
