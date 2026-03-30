# Native Analog Video: 384-Native TV Mode Plan

## Changelog

### 2026-03-20 (rev 2)

- Fix diagonal moving color bands on S-Video: change NTSC pixel clock from 7.553 MHz to
  `7.552446593 MHz` (satisfies `fsc = 227.5 × line_rate` exactly for `htotal=480`).
  Change PAL pixel clock from 7.553 MHz to `7.5 MHz` (restores the exact 15,625 Hz PAL
  line rate). Both values are PLL-exact.

### 2026-03-20

- Preserve `forced_scandoubler` by keeping scandoubled native-analog outputs on the existing
  31 KHz TV-mode family; only non-scandoubled native-analog outputs switch to 384-native.
- Correct NTSC/PAL timing math and beat estimates, and downgrade judder/spec language to
  inference plus hardware validation.

## Problem

S-Video (and other non-scandoubled native analog outputs) currently forces a 640×240 NTSC /
640×288 PAL
framebuffer. The game renders at 384 pixels wide, so the presenter must scale 384→640
horizontally — a non-integer 1.667× ratio. This produces a [1,2] pixel-width alternation
pattern visible as waviness on fine horizontal detail.

Component with an explicit `video_conf=384x224` + "native" scale looked pixel-perfect by
contrast, because the framebuffer matched the game's native width and the presenter did a
1:1 copy with no scaling.

Real FPGA cores look perfect because the game's dot clock IS the DAC output clock —
pixels are output one-for-one with no framebuffer re-timing. The HPS path can match this:
if the framebuffer width equals the game's native pixel width and the output pixel clock
is set to maintain valid analog line timing, the result is identical to a real core.

## Solution

Replace the 640-wide forced TV mode for non-scandoubled native analog outputs (component,
S-Video, CVBS — `vga_mode_int` 1/2/3 with `forced_scandoubler=0`) with a 384-wide mode that:

- matches the game's native horizontal resolution (zero horizontal scaling)
- maintains the same physical CRT active line width (50.84 µs — identical to 640×240)
- keeps valid NTSC/PAL line rates so real CRTs sync correctly
- lets `fb_native_analog_auto` in `set_yc_mode()` calculate the correct S-Video
  `PHASE_INC` for the new pixel clock automatically
- moves the output refresh much closer to the game's target framerate
  (`TARGET_FPS = 59.59949`), reducing the estimated refresh beat from ~2.2 seconds to
  ~3.8 minutes

Keep SDL scale-mode handling on the regular active modes only — non-scandoubled
native-analog paths now use ordinary "native" scale with a game-resolution
framebuffer.

## Timing Rationale

### 640×240 NTSC 15K (existing)
```
hact=640  hfp=30  hs=60  hbp=70  htotal=800   Fpix=12.587 MHz
vact=240  vfp=4   vs=4   vbp=14  vtotal=262
line_rate = 12.587 MHz / 800 = 15,734 Hz
active_time = 640 / 12.587 MHz = 50.84 µs
```

### 384×240 NTSC 15K 384-native (new)
```
hact=384  hfp=18  hs=36  hbp=42  htotal=480   Fpix=7.552446593 MHz
vact=240  vfp=6   vs=4   vbp=14  vtotal=264
line_rate = 7.552446593 MHz / 480 = 15,734.26 Hz  ✓ (NTSC standard 15,734.264 Hz)
active_time = 384 / 7.552446593 MHz = 50.84 µs    ✓ (same physical CRT width)
fps = 7.552446593M / (480 × 264) ≈ 59.596 Hz      ✓ (TARGET_FPS=59.59949, Δ≈0.003 Hz)
pal detection: 59.596 > 55 → pal=0                ✓
fsc / line_rate = 3,579,545 / 15,734.26 = 227.500  ✓ (NTSC half-cycle, exact)
```

The NTSC half-cycle constraint (`fsc = 227.5 × line_rate`) requires the pixel clock to
satisfy `fsc × htotal / Fpix = 227.5` exactly. Rearranged: `Fpix = fsc × htotal / 227.5
= 3.579545 × 480 / 227.5 = 7.552446593... MHz`. With the original 7.553 MHz the error
was 0.016 cycles/line (5.8°/line). With the rounded 7.5524 it was 0.505°/line. The exact
value `7.552446593` is used because MiSTer's PLL search can hit it exactly, reducing the
per-line burst phase error to zero. vtotal=264 moves the output refresh closer to
`TARGET_FPS = 59.59949` (`sdl_app.h:7`): the existing 640×240 mode is `12.587e6 / (800 ×
262) ≈ 60.053 Hz` (`Δ≈0.453 Hz`, estimated beat ≈`2.2 s`) and the 384×240 mode is
`7.552447e6 / (480 × 264) ≈ 59.596 Hz` (`Δ≈0.003 Hz`, estimated beat ≈`333 s` /
`5.6 min`). Beat figures are inferences, not hardware-proven judder measurements. Whether
264-line progressive timing locks cleanly across real CRTs is a hardware-validation gate.

### 384×288 PAL 15K 384-native (new)
```
hact=384  hfp=18  hs=36  hbp=42  htotal=480   Fpix=7.5 MHz
vact=288  vfp=6   vs=4   vbp=14  vtotal=312
line_rate = 7.5 MHz / 480   = 15,625 Hz        ✓ (PAL standard 15,625 Hz)
active_time = 384 / 7.5 MHz = 51.2 µs          ✓
fps = 7.5M / (480 × 312)    ≈ 50.08 Hz         ✓ → pal=1
fsc / line_rate = 4,433,619 / 15,625 = 283.7516  ✓
```

`7.5 MHz` restores the exact PAL line rate (`15,625 Hz`) and the PLL achieves it exactly
(`c=54, m=8, ko=0.1 → Fpix_actual = 7.500 MHz`). The resulting ratio is
`4.43361875 / 15625 = 283.7516`, which is the expected PAL subcarrier relationship for
this line rate.

Blanking values are proportionally scaled from the 640-wide mode (factor 96/160 = 0.6):
hfp=18, hs=36, hbp=42. Colorburst window (calculated automatically by `set_yc_mode()`):
NTSC: start≈7px, end≈25px — fits within hbp=42px.
PAL:  start≈6px, end≈21px — fits within hbp=42px.

Each game pixel now occupies ~132 ns on the DAC (vs 79.4 ns at 640×240). The image
fills the same horizontal extent on the CRT — pixels are wider, not the image.
This is identical to what a real FPGA core produces for a 384-pixel-wide game at ~7.5 MHz.

Note on PAL ratio: `4.43361875 / 15625 = 283.7516`, not flat 283.75. 7.5 MHz is still
the right clock (it achieves line_rate = 15,625 Hz exactly, which is the defining PAL
constraint), but the subcarrier-to-line ratio is 283.7516 rather than a round 283.75.

## Signal Chain After Change

```
ARM renders 384×224
  → fbdev presenter 1:1 copy (no scaling)
  → /dev/fb0 (384×240)
  → FPGA reads at 7.552446593 MHz (384 px/line)
  → yc_out_fb encodes at 7.552446593 MHz
  → PHASE_INC = (3.579545 / 7.552446593) × 2^40 = (227.5/480) × 2^40 ≈ 521.5B  [NTSC, exact]
  → DAC → S-Video / component / CVBS CRT
```

## Files Changed

| File | Change |
|------|--------|
| `vendor/Main_MiSTer/video.cpp` | Add 384-native TV modes, add setter, extend `should_use_native_analog_tv_mode()`, update `video_mode_load()` |
| `vendor/Main_MiSTer/threesx_wrapper.cpp` | Simplify native analog auto-select to always return "native" |
| `src/port/sdl/sdl_app.c` | Remove the obsolete dedicated native-analog scale mode and simplify `fit_native_rect()` |

---

## Step 1 — Add TV mode entries (`video.cpp` line ~132)

Append two entries to `tvmodes[]`:

```cpp
vmode_t tvmodes[] =
{
    {{ 640, 30, 60, 70, 240,  4, 4, 14 }, 12.587, 0, 0 }, // NTSC 15K
    {{ 640, 16, 96, 48, 480,  8, 4, 33 }, 25.175, 0, 0 }, // NTSC 31K
    {{ 640, 30, 60, 70, 288,  6, 4, 14 }, 12.587, 0, 0 }, // PAL  15K
    {{ 640, 16, 96, 48, 576,  2, 4, 42 }, 25.175, 0, 0 }, // PAL  31K
    {{ 384, 18, 36, 42, 240,  6, 4, 14 },  7.552446593, 0, 0 }, // NTSC 15K 384-native (fsc=3.579545×480/227.5)
    {{ 384, 18, 36, 42, 288,  6, 4, 14 },  7.5,         0, 0 }, // PAL  15K 384-native (line_rate=15625 Hz)
};
```

---

## Step 2 — Add index function and setter (`video.cpp`, after `default_tv_mode_index`)

```cpp
static int native_analog_tv_mode_index()
{
    return cfg.menu_pal ? 5 : 4;
}

static void set_native_analog_tv_video_mode()
{
    const int mode = native_analog_tv_mode_index();

    memset(&v_def, 0, sizeof(v_def));
    v_def.item[0] = mode;
    for (int i = 0; i < 8; i++) v_def.item[i + 1] = tvmodes[mode].vpar[i];
    setPLL(tvmodes[mode].Fpix, &v_def);

    vmode_def = 1;
    vmode_pal = 0;
    vmode_ntsc = 0;
}
```

---

## Step 3 — Extend `should_use_native_analog_tv_mode()` (`video.cpp` line ~2496)

Include component (mode 1), but preserve the scan-doubled path by excluding
`forced_scandoubler` from the new 384-native mode:

```cpp
static bool should_use_native_analog_tv_mode()
{
    if (cfg.direct_video) return false;
    if (cfg.vga_scaler) return false;
    if (cfg.forced_scandoubler) return false;
    return (cfg.vga_mode_int == 1) || (cfg.vga_mode_int == 2) || (cfg.vga_mode_int == 3);
}
```

---

## Step 4 — Update `video_mode_load()` (`video.cpp` line ~2545)

```cpp
else if (!has_explicit_video_mode_override() &&
         !cfg.direct_video &&
         !cfg.vga_scaler &&
         ((cfg.vga_mode_int == 1) || (cfg.vga_mode_int == 2) || (cfg.vga_mode_int == 3)) &&
         cfg.forced_scandoubler)
{
    printf("video_mode_load: using scandoubled TV mode family for vga_mode=%s (mode=%d)\n",
           cfg.vga_mode,
           default_tv_mode_index());
    set_default_tv_video_mode();      // preserve existing 31 KHz behavior
}
else if (should_use_native_analog_tv_mode() && !has_explicit_video_mode_override())
{
    printf("video_mode_load: using 384-native analog TV mode for vga_mode=%s (mode=%d)\n",
           cfg.vga_mode,
           native_analog_tv_mode_index());
    set_native_analog_tv_video_mode();  // was: set_default_tv_video_mode()
}
```

---

## Step 5 — Simplify scale mode auto-select (`threesx_wrapper.cpp` line ~642)

The branch now always returns "native" for the non-scandoubled native-analog case — no
need to split on mode 1 vs 2/3:

```cpp
else if ((selection.io_type == 0) && (selection.vga_scaler == 0) && (selection.forced_scandoubler == 0) &&
         is_native_analog_tv_output_mode(selection.vga_mode_int))
{
    // Non-scandoubled native analog outputs (component/S-Video/CVBS) use 384-native TV mode.
    // Framebuffer matches game native width → 1:1 presenter copy → no scaling artifacts.
    snprintf(selection.source, sizeof(selection.source), "auto-native-analog");
    snprintf(selection.value, sizeof(selection.value), "native");
}
```

---

## Step 6 — Simplify SDL scale-mode handling (`sdl_app.c`)

Remove the obsolete dedicated native-analog scale mode from all locations:

- remove the extra enum/parser/string/filter cases for the old dedicated mode
- route native analog output through ordinary `native` scale handling
- delete any one-off letterbox helpers that only existed for that removed mode

---

## Automatic PHASE_INC Handling (No Code Change Required)

`fb_native_analog_auto` in `set_yc_mode()` (`video.cpp`) activates when:
- `vga_fb=1` (HPS framebuffer path)
- `should_use_native_analog_tv_mode()=true` (now includes non-scandoubled mode 1)
- `v_cur.Fpix > 0`

With the new mode, `v_cur.Fpix = 7.553 MHz`. The existing formula gives:

```
PHASE_INC = (3.579545 / 7.553) × 2^40 ≈ 520,731,000,000  [NTSC]
PHASE_INC = (4.433619 / 7.553) × 2^40 ≈ 645,116,000,000  [PAL]
```

PAL/NTSC detection at `vtime=0` (before game starts) uses output timing:
- NTSC: fps = 7.553M/(480×264) ≈ 59.604 Hz > 55 → pal=0  ✓
- PAL:  fps = 7.553M/(480×312) ≈ 50.434 Hz < 55 → pal=1  ✓

No changes needed to `set_yc_mode()` or `video_refresh_yc_mode()`.

---

## Risks to Verify on Hardware

**1. `fb_width` / `fb_height` in `video_fb_enable()`**
These are read from the Linux `/dev/fb0` after `video_set_mode()` programs the FPGA with
new timing. Verify the kernel reports 384×240 after the mode switch. The FPGA viewport
(`v_cur.item[1]`=384 hact, `v_cur.item[5]`=240 vact) is set correctly regardless.

**2. `tvmodes[]` size assumptions**
Grep for any code that hardcodes the array size (e.g. `sizeof(tvmodes)/sizeof(tvmodes[0])`
comparisons or loop bounds) before building.

**3. CRT sync lock at 7.553 MHz / htotal=480**
Line rate = 15,735 Hz stays close to NTSC M. Most S-Video and component CRTs are expected
to lock to this, but the 264-line progressive timing remains a hardware-validation gate.
First hardware test should confirm sync before declaring the change stable.

**4. Component users on modern displays**
Users running component to a 480p/720p-capable modern display with non-scandoubled native
analog output will now receive a 240p signal instead of the previous EDID-derived
resolution. Anyone using native analog (no scaler, no scandoubler) is expected to be on a
CRT — this should not affect real users, but note the behavioral change.
