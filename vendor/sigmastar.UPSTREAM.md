# SigmaStar MI_SDK Vendor Metadata

- Source upstream: `https://github.com/steward-fu/sdl2.git` (`mini/inc/`
  and `mini/lib/` subdirectories of an LGPL-2.1 SDL2 fork that
  redistributes the SigmaStar BSP headers + prebuilt `.so` files for
  the SSD202D / Miyoo Mini Plus platform).
- Pinned commit (snapshot taken 2026-05-03):
  `0631abc8e8916db6f9bc7e2afd0c22913d092a29`.
- Local vendor path: `vendor/sigmastar/`.
- Files vendored:
  - `vendor/sigmastar/inc/` (nine headers — six used by the Cut 1
    MI_GFX presenter, plus three for the direct MI_AO audio backend
    that replaces the broken SDL3-OSS path on this device):
    - `mi_sys.h`
    - `mi_sys_datatype.h`
    - `mi_common.h`
    - `mi_common_datatype.h`
    - `mi_gfx.h`
    - `mi_gfx_datatype.h`
    - `mi_ao.h`            (added for direct MI_AO audio backend)
    - `mi_ao_datatype.h`   (added for direct MI_AO audio backend)
    - `mi_aio_datatype.h`  (added for direct MI_AO audio backend)
    - Licensing posture for the three MI_AO additions is identical to
      the pre-existing SigmaStar headers — see Licensing posture below.
      Re-review needed if SigmaStar issues a takedown.
  - `vendor/sigmastar/lib/` (four prebuilt ARMv7 hardfloat shared
    objects — the link-time NEEDED set of the MI_GFX presenter plus
    the MI_AO audio backend):
    - `libmi_sys.so`     (60880B)
    - `libmi_gfx.so`     (27612B)
    - `libmi_common.so` (138696B)
    - `libmi_ao.so`     (151876B; added for direct MI_AO audio backend)
    - Licensing posture for `libmi_ao.so` is identical to the
      pre-existing SigmaStar libs — see Licensing posture below.
      Re-review needed if SigmaStar issues a takedown.
  - The bare `.so` filenames are deliberate. The Miyoo Mini Plus
    OnionOS BSP exposes `/config/lib/libmi_*.so` (no `.so.0` SONAME
    suffix — verified on a v0.4-class Onion install via on-device
    probe). The vendored stubs have no DT_SONAME, so linking against
    them produces binaries whose NEEDED entries say `libmi_sys.so`
    (etc.), matching the runtime-resolved filename one-for-one. An
    earlier Cut 1 attempt added `.so.0` symlinks under the assumption
    that the BSP used SONAME suffixes — that was wrong; the symlinks
    were removed.

## Import intent

The project's `arm_display_mi_gfx.c` presenter on the Miyoo Mini Plus
profile (`PORT_MIYOO_MINI_PLUS=ON`) drives the SigmaStar MI_GFX
hardware blitter via the SigmaStar BSP user-space libraries. The
vendored headers expose the MI_SYS / MI_GFX / MI_COMMON entry points
(`MI_SYS_Init`, `MI_SYS_Mmap`, `MI_SYS_MMA_Alloc`, `MI_GFX_Open`,
`MI_GFX_BitBlit`, `MI_GFX_WaitAllDone`, etc.). The vendored `.so`
files satisfy the link-time symbol references against those headers.

The audio backend at `src/port/sound/mi_ao_backend.c` drives the
SigmaStar MI_AO HAL directly, bypassing SDL3 audio + the kernel
`/dev/dsp` OSS shim entirely. The MI_AO headers expose
`MI_AO_SetPubAttr`, `MI_AO_Enable`, `MI_AO_EnableChn`,
`MI_AO_SendFrame`, etc.; `libmi_ao.so` satisfies those at link time
and is resolved at runtime from `/config/lib/libmi_ao.so` on the
device, same as the other libmi_*.so members.

All wrapper code in `arm_display_mi_gfx.c` and
`src/port/sound/mi_ao_backend.c` is authored by us against the
vendored header signatures; no code is copied from steward-fu's SDL2
fork — that repo was read structurally as a reference for how to
sequence the MI_SYS / MI_GFX / MI_AO calls.

## Licensing posture

The SigmaStar headers carry a proprietary banner from Sigmastar
Technology Corp. (2018-2019) describing them as "Sigmastar
Confidential Information" with redistribution "unlawful and strictly
prohibited". Despite that banner, redistribution of these headers and
the prebuilt `.so` files is the universal pattern across every public
OnionOS-adjacent port and has been for 5+ years with zero observed
enforcement:

- **steward-fu/sdl2** vendors the headers in `mini/inc/` and the libs
  in `mini/lib/` under an LGPL-2.1 SDL2 fork.
- **OnionUI Sonic Mania port** (Ports-Collection) ships a binary
  built against steward-fu's `mini/lib/`.
- **XK9274/sdl2_miyoo, OOPay/sdl2, MyMinUI** all follow the same
  pattern.

We adopt that pattern. This is a deliberate, documented stance: we
follow the established community practice for an AGPLv3 hobbyist
project. If SigmaStar ever issues a takedown, the response is to
remove `vendor/sigmastar/`, switch to a `dlopen()`-at-runtime strategy
with shims authored from public docs or reverse-engineered, and
continue.

## Runtime expectation

Two distinct distribution surfaces:

1. **Build-time link.** The headers and prebuilt `.so` files are
   committed under `vendor/sigmastar/{inc,lib}/` and consumed by
   cmake at link time when `PORT_MIYOO_MINI_PLUS=ON`. Contributors
   clone the repo, build, no extra setup or env-var contract.

2. **Runtime resolution.** The deployed package's `lib/` directory
   does **not** bundle `libmi_*.so`. The 3s-arm binary's NEEDED
   entries (`libmi_sys.so`, `libmi_gfx.so`, `libmi_common.so`)
   resolve at runtime against the device's `/config/lib/`,
   populated by the OnionOS firmware BSP image on internal flash.
   This matches the universal community deployment pattern (verified
   by inspecting the OnionUI Sonic Mania port's deployed `lib/` —
   contains libSDL2 + libtheora + libjson-c + libz, NOT `libmi_*`,
   and its bundled libSDL2 has bare-name `libmi_*` NEEDED entries).

   **ABI fallback** (rare but real): if a future Onion firmware
   ships the libs with a different name pattern (`.so.0` SONAME or
   different basename), a fast-follow patch can bundle the on-device
   `.so` for that firmware version into the deployed `lib/`. Tracked
   as a low-likelihood, low-impact risk.

## Onion target release pin

Cut 1 targets the latest stable Onion as of 2026-05-03. The runtime
contract assumes the stock Onion `/config/lib/` exposes
`libmi_sys.so`, `libmi_gfx.so`, and `libmi_common.so` (bare names,
no SONAME suffix) matching the ABI of the vendored headers. If a
contributor verifies a mismatch on an older / experimental Onion
build, the response is the ABI fallback above — bundle the on-device
`.so` for that target.

## Toolchain pin

The Buildroot-glibc cross-compile container is built from upstream
`shauninman/union-miyoomini-toolchain`. The default SHA pinned in
`tools/miyoo/setup-build-container.sh` is:

- `cfcae3083679de59d6600d4ba05676c654b6d24e` (tip of `main` as of
  2026-05-03; the upstream's actual default branch is `main`, not
  `master`).

Pinning a SHA — not a branch — guarantees the toolchain that produced
a shipped binary is reproducible. To re-pin, run:

```sh
gh api repos/shauninman/union-miyoomini-toolchain/commits/main --jq .sha
```

and update both the default in `setup-build-container.sh` and this
document in the same commit.

## Upstream header note

`vendor/sigmastar/inc/mi_gfx_datatype.h` line 21 self-includes
`mi_gfx_datatype.h`. This is an upstream defect carried verbatim from
`steward-fu/sdl2`'s `mini/inc/`; the header guard prevents recursion
so the TU compiles cleanly. Vendored as-is so the import is a literal
copy of upstream — do not "fix" it locally and call it a corruption
fix later.

## Re-import procedure

1. `git clone https://github.com/steward-fu/sdl2 /tmp/sdl2-mmiyoo`.
2. Pin a commit, record the SHA at the top of this file.
3. Copy the six headers from `mini/inc/` into `vendor/sigmastar/inc/`.
4. Copy the three `.so` files from `mini/lib/` into
   `vendor/sigmastar/lib/`.
5. Update `THIRD_PARTY_NOTICES.txt` if attribution changes.
