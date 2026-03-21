# Native Analog Video: 384-Native TV Mode Plan

## Problem

S-Video (and all native analog outputs) currently forces a 640×240 NTSC / 640×288 PAL
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

Replace the 640-wide forced TV mode for all native analog outputs (component, S-Video,
CVBS — `vga_mode_int` 1/2/3) with a 384-wide mode that:

- matches the game's native horizontal resolution (zero horizontal scaling)
- maintains the same physical CRT active line width (50.84 µs — identical to 640×240)
- keeps valid NTSC/PAL line rates so real CRTs sync correctly
- lets `fb_native_analog_auto` in `set_yc_mode()` calculate the correct S-Video
  `PHASE_INC` for the new pixel clock automatically
- matches the game's target framerate (`TARGET_FPS = 59.59949`) by tuning vtotal,
  reducing vsync judder from one event every ~2 seconds to one every ~140 seconds

Remove `SCALEMODE_CRT_4X3` entirely — it was only ever auto-selected for native analog
paths, which are all replaced by "native" scale with a game-resolution framebuffer.

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
hact=384  hfp=18  hs=36  hbp=42  htotal=480   Fpix=7.553 MHz
vact=240  vfp=6   vs=4   vbp=14  vtotal=264
line_rate = 7.553 MHz / 480  = 15,735 Hz       ✓ (NTSC tolerance)
active_time = 384 / 7.553 MHz = 50.84 µs       ✓ (same physical CRT width)
fps = 7.553M / (480 × 264)   = 59.592 Hz       ✓ (TARGET_FPS=59.59949, Δ=0.007 Hz)
pal detection: 59.592 > 55 → pal=0             ✓
```

vtotal=264 (not the standard 262) is chosen to match `TARGET_FPS = 59.59949`
(`sdl_app.h:7`). The existing 640×240 mode uses vtotal=262 → 60.05 Hz, which
mismatches TARGET_FPS by 0.45 Hz and causes a visible judder event roughly every 2
seconds. vtotal=264 reduces this to 0.007 Hz mismatch — one judder every ~140 seconds.
The 2 extra vertical blank lines go into vfp (6 instead of 4); this is within NTSC spec.

### 384×288 PAL 15K 384-native (new)
```
hact=384  hfp=18  hs=36  hbp=42  htotal=480   Fpix=7.553 MHz
vact=288  vfp=6   vs=4   vbp=14  vtotal=312
line_rate = 7.553 MHz / 480  = 15,735 Hz       ✓
active_time = 384 / 7.553 MHz = 50.84 µs       ✓
fps = 7.553M / (480 × 312)   = 50.40 Hz        ✓ → pal=1
```

Blanking values are proportionally scaled from the 640-wide mode (factor 96/160 = 0.6):
hfp=18, hs=36, hbp=42. Colorburst window (calculated automatically by `set_yc_mode()`):
start≈7px, end≈25px — fits within hbp=42px.

Each game pixel now occupies 132 ns on the DAC (vs 79.4 ns at 640×240). The image
fills the same horizontal extent on the CRT — pixels are wider, not the image.
This is identical to what a real FPGA core produces for a 384-pixel-wide game at ~7.5 MHz.

## Signal Chain After Change

```
ARM renders 384×224
  → fbdev presenter 1:1 copy (no scaling)
  → /dev/fb0 (384×240)
  → FPGA reads at 7.552 MHz (384 px/line)
  → yc_out_fb encodes at 7.552 MHz
  → PHASE_INC = (3.579545 / 7.553) × 2^40 ≈ 520.7B  [NTSC, automatic]
  → DAC → S-Video / component / CVBS CRT
```

## Files Changed

| File | Change |
|------|--------|
| `vendor/Main_MiSTer/video.cpp` | Add 384-native TV modes, add setter, extend `should_use_native_analog_tv_mode()`, update `video_mode_load()` |
| `vendor/Main_MiSTer/threesx_wrapper.cpp` | Simplify native analog auto-select to always return "native" |
| `src/port/sdl/sdl_app.c` | Remove `SCALEMODE_CRT_4X3` everywhere, extend `is_native_analog_tv_framebuffer_size()` |

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
    {{ 384, 18, 36, 42, 240,  6, 4, 14 },  7.553, 0, 0 }, // NTSC 15K 384-native (3SX analog)
    {{ 384, 18, 36, 42, 288,  6, 4, 14 },  7.553, 0, 0 }, // PAL  15K 384-native (3SX analog)
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

Include component (mode 1) so all three native analog outputs share the same TV mode path:

```cpp
static bool should_use_native_analog_tv_mode()
{
    if (cfg.direct_video) return false;
    if (cfg.vga_scaler) return false;
    return (cfg.vga_mode_int == 1) || (cfg.vga_mode_int == 2) || (cfg.vga_mode_int == 3);
}
```

---

## Step 4 — Update `video_mode_load()` (`video.cpp` line ~2545)

```cpp
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

The branch now always returns "native" — no need to split on mode 1 vs 2/3:

```cpp
else if ((selection.io_type == 0) && (selection.vga_scaler == 0) && (selection.forced_scandoubler == 0) &&
         is_native_analog_tv_output_mode(selection.vga_mode_int))
{
    // All native analog outputs (component/S-Video/CVBS) use 384-native TV mode.
    // Framebuffer matches game native width → 1:1 presenter copy → no scaling artifacts.
    snprintf(selection.source, sizeof(selection.source), "auto-native-analog");
    snprintf(selection.value, sizeof(selection.value), "native");
}
```

---

## Step 6 — Remove `SCALEMODE_CRT_4X3` (`sdl_app.c`)

Remove from all locations:

- `enum ScaleMode` — remove `SCALEMODE_CRT_4X3` entry
- `scale_mode_name()` — remove `case SCALEMODE_CRT_4X3: return "crt-4x3";`
- `parse_scale_mode()` — remove `"crt-4x3"` branch
- `sdl_scale_mode_filter()` — remove `case SCALEMODE_CRT_4X3:` from nearest group
- `scale_mode_uses_native_render_path()` — remove `SCALEMODE_CRT_4X3` from condition
- `get_letterbox_rect()` — remove `case SCALEMODE_CRT_4X3:` and `fit_crt_4x3_rect()` call
- `fit_crt_4x3_rect()` — delete the function entirely

---

## Step 7 — Extend `is_native_analog_tv_framebuffer_size()` (`sdl_app.c` line ~7113)

Add 384-wide recognition (for correctness in any remaining code that checks this):

```c
static bool is_native_analog_tv_framebuffer_size(int win_w, int win_h) {
#if defined(PORT_MISTER)
    if (win_w == 640) {
        return (win_h == 240) || (win_h == 288) || (win_h == 480) || (win_h == 576);
    }
    if (win_w == 384) {
        return (win_h == 240) || (win_h == 288);
    }
    return false;
#else
    (void)win_h;
    return false;
#endif
}
```

---

## Automatic PHASE_INC Handling (No Code Change Required)

`fb_native_analog_auto` in `set_yc_mode()` (`video.cpp`) activates when:
- `vga_fb=1` (HPS framebuffer path)
- `should_use_native_analog_tv_mode()=true` (now includes mode 1)
- `v_cur.Fpix > 0`

With the new mode, `v_cur.Fpix = 7.553 MHz`. The existing formula gives:

```
PHASE_INC = (3.579545 / 7.553) × 2^40 ≈ 520,731,000,000  [NTSC]
PHASE_INC = (4.433619 / 7.553) × 2^40 ≈ 645,116,000,000  [PAL]
```

PAL/NTSC detection at `vtime=0` (before game starts) uses output timing:
- NTSC: fps = 7.553M/(480×264) = 59.592 Hz > 55 → pal=0  ✓
- PAL:  fps = 7.553M/(480×312) = 50.40 Hz  < 55 → pal=1  ✓

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

**3. CRT sync lock at 7.552 MHz / htotal=480**
Line rate = 15,733 Hz is within NTSC tolerance. Most S-Video and component CRTs lock to
this fine. First hardware test should confirm sync before declaring the change stable.

**4. Component users on modern displays**
Users running component to a 480p/720p-capable modern display will now receive a 240p
signal instead of the previous EDID-derived resolution. Anyone using native analog
(no scaler, no scandoubler) is expected to be on a CRT — this should not affect real
users, but note the behavioral change.
