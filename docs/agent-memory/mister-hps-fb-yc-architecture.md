# MiSTer Analog Video Output: Deep Reference

When to load:
- Load this when working on S-Video, CVBS, or native analog CRT output for MiSTer or 3SX — video pipeline routing, YC encoding, PHASE_INC tuning, DAC mux, or framebuffer-to-analog handoff.

---

## The Two Video Pipelines

MiSTer has two parallel video paths that share the same DAC output pins. Only one is active at a time.

### Pipeline A — FPGA core video
```
FPGA game core VGA signals
  → scanlines module
  → OSD overlay                  (vga_data_osd)
  → yc_out module                ← YC/CVBS subcarrier encoder
  → vga_out                      (vga_o[23:0])
  → DAC                          [active when vgas_en=0]
```
- Clock domain: `clk_vid` (core dot clock — varies per game/mode)
- YC encoding happens here via `yc_out.sv`
- Normal path for all FPGA-native game cores

### Pipeline B — HPS framebuffer
```
/dev/fb0 (HPS DDR RAM)
  → ascal (hardware scaler, HPS DMA)
  → shadowmask
  → HDMI OSD overlay             (hdmi_data_osd)
  → vga_scaler_out               (vgas_o[23:0])   ← plain RGB, no YC
  → DAC                          [active when vgas_en=1]
```
- Clock domain: `clk_hdmi` (scaler clock — typically fixed or output-mode-driven)
- No YC encoder in this chain by default
- Used by 3SX (HPS software renderer), menu OSD, scaler modes

---

## The DAC Mux (`sys_top.v`)

```verilog
wire vgas_en = vga_fb | vga_scaler;

assign VGA_R = vgas_en ? vgas_o[23:18] : vga_o[23:18];
assign VGA_G = vgas_en ? vgas_o[15:10] : vga_o[15:10];
assign VGA_B = vgas_en ? vgas_o[7:2]   : vga_o[7:2];
```

- `vga_fb = cfg[12]` — set by HPS via `set_vga_fb(1)` → `spi_uio_cmd16(UIO_BUT_SW, map)` with bit 12 set
- `vga_scaler = cfg[2] | vga_force_scaler` — Menu core always assigns `VGA_SCALER=0`
- When `vga_fb=1` (3SX framebuffer mode): `vgas_en=1` → DAC gets Pipeline B output

### VGA pixel clock also switches
```verilog
cyclonev_clkselect vga_clk_sw (
    .clkselect({1'b1, ~vga_fb & ~vga_scaler}),
    .inclk({clk_vid, hdmi_clk_out, 2'b00}),
    ...
);
```
When `vga_fb=1`: VGA output pins run on `hdmi_clk_out` (scaler timing), not `clk_vid`.

---

## YC Encoder: `yc_out.sv`

Module ports:
- `PHASE_INC[39:0]` — subcarrier NCO increment (determines subcarrier frequency)
- `PAL_EN` — enables PAL phase-alternating line algorithm; use 0 for NTSC
- `CVBS` — if 1, suppresses chroma output (luma-only / composite-ready)
- `COLORBURST_RANGE[16:0]` — `{start[6:0], end[9:0]}` packed; defines horizontal window for colorburst
- `hsync` input — active-HIGH; resets colorburst phase counter each line
- `dout[23:0]` — `{C[7:0], Y[7:0], 8'd0}` packed output

### NCO (subcarrier generation)
```verilog
phase_accum <= phase_accum + PHASE_INC;
chroma_LUT = phase_accum[39:32];   // top 8 bits index into sine table
```
The subcarrier frequency is: `f_sub = PHASE_INC / 2^40 * CLK_VIDEO`

### PAL phase alternation
When `PAL_EN=1`: the V component of chroma flips sign each line (standard PAL). Set `PAL_EN=0` for NTSC. Using `PAL_EN=1` on an NTSC TV is a primary failure mode — TV colorburst PLL cannot lock.

### Output packing
```verilog
assign dout = {C, Y, 8'd0};
```
Y on bits [15:8], C on bits [23:16]. The DAC only uses the top 6 bits of each channel.

---

## The `yc_out_fb` Second Encoder (3SX Fix)

To support HPS framebuffer YC output (3SX native analog), `sys_top.v` was modified to add a second encoder instance on Pipeline B's clock domain.

### New wires
```verilog
wire [23:0] yc_fb_o;
wire        yc_fb_hs, yc_fb_vs, yc_fb_cs, yc_fb_de;
wire        vga_fb_yc_en = vga_fb & ~vga_scaler & yc_en;
```

`vga_fb_yc_en` is true when:
- `vga_fb=1` (HPS framebuffer active)
- `vga_scaler=0` (scaler not overriding — always 0 in Menu/3SX native analog)
- `yc_en=1` (YC mode enabled, set by `UIO_SET_YC_PAR`)

### `yc_out_fb` instance
```verilog
yc_out yc_out_fb (
    .clk        (clk_hdmi),          // ← scaler clock domain, not clk_vid
    .reset      (reset),
    .PAL_EN     (pal_en),
    .CVBS       (cvbs),
    .PHASE_INC  (phase_inc),
    .COLORBURST_RANGE(colorburst_range),
    .R          (hdmi_data_osd[23:16]),
    .G          (hdmi_data_osd[15:8]),
    .B          (hdmi_data_osd[7:0]),
    .hs_in      (hdmi_hs_osd),       // active-HIGH internally
    .vs_in      (hdmi_vs_osd),
    .de_in      (hdmi_de_osd),
    .dout       (yc_fb_o),
    ...
);
```

### Updated DAC mux
```verilog
assign VGA_R = vga_fb_yc_en ? yc_fb_o[23:18] : vgas_en ? vgas_o[23:18] : vga_o[23:18];
assign VGA_G = vga_fb_yc_en ? yc_fb_o[15:10] : vgas_en ? vgas_o[15:10] : vga_o[15:10];
assign VGA_B = vga_fb_yc_en ? yc_fb_o[7:2]   : vgas_en ? vgas_o[7:2]   : vga_o[7:2];
```
`vga_fb_yc_en` takes priority over both `vgas_en` and the core path.

### Subcarrier gating
```verilog
subcarrier_out <= ~(subcarrier & csync_en & ~ypbpr_en & ~forced_scandoubler
                    & ~(vgas_en & ~vga_fb_yc_en)) | sub_accum[39];
```
Previously `~vgas_en` suppressed the subcarrier pin entirely when `vgas_en=1`. The new condition allows the subcarrier through when `vga_fb_yc_en=1`.

---

## HPS Side: `set_yc_mode()` in `video.cpp`

Called from `video_refresh_yc_mode()` → called from `threesx_wrapper.cpp` just before `fork()+execve()` launches the game.

### PAL/NTSC determination
```cpp
float fps = current_video_info.vtime ? (100000000.f / current_video_info.vtime) : 0.f;
int pal = fps < 55.f;
```
`current_video_info` is populated from the FPGA via `get_video_info()` during game runtime. **Before the game starts, `vtime=0` → `fps=0 < 55 → pal=1` (wrong for NTSC).** This was a primary failure mode.

### `fb_native_analog_auto` — output clock selection
```cpp
const bool vga_fb_enabled = get_vga_fb();
const bool native_analog_tv_mode = should_use_native_analog_tv_mode();
const bool output_clock_available = v_cur.Fpix > 0.0;
const bool fb_native_analog_auto = output_clock_available
    && vga_fb_enabled
    && native_analog_tv_mode;
if (fb_native_analog_auto) {
    CLK_VIDEO = v_cur.Fpix;   // use output pixel clock, not core pixel clock
}
```
`should_use_native_analog_tv_mode()`: true when `!direct_video && !vga_scaler && (vga_mode_int==2||3)`

`v_cur.Fpix` is set at startup by `video_init()` → `set_default_tv_video_mode()` → `setPLL(12.587, ...)`. It reflects the currently programmed output pixel clock in MHz.

### PAL/NTSC correction for `vtime=0` case
When `fb_native_analog_auto=true` and `vtime=0`, derive PAL/NTSC from output timing instead:
```cpp
if (fb_native_analog_auto && !current_video_info.vtime) {
    int htotal = v_cur.param.hact + v_cur.param.hfp + v_cur.param.hs + v_cur.param.hbp;
    int vtotal = v_cur.param.vact + v_cur.param.vfp + v_cur.param.vs + v_cur.param.vbp;
    if (htotal > 0 && vtotal > 0) {
        double output_fps = (v_cur.Fpix * 1000000.0) / ((double)htotal * vtotal);
        pal = output_fps < 55.0;
        CLK_REF = (pal || (cfg.ntsc_mode == 1)) ? 4.43361875 : (cfg.ntsc_mode == 2) ? 3.575611 : 3.579545;
    }
}
```

### PHASE_INC formula
```cpp
int64_t PHASE_INC = ((int64_t)((CLK_REF / CLK_VIDEO) * 1099511627776LL)) & 0xFFFFFFFFFFLL;
```
Where `1099511627776 = 2^40`.

**Reference clocks:**
- NTSC: `CLK_REF = 3.579545 MHz`
- PAL: `CLK_REF = 4.43361875 MHz`
- PAL-M: `CLK_REF = 3.575611 MHz`

**NTSC 15K example:** `CLK_VIDEO = 12.587 MHz` → `PHASE_INC = (3.579545 / 12.587) × 2^40 ≈ 312,741,000,000`

**Wrong PAL example (the bug):** `CLK_VIDEO = 12.587 MHz`, `CLK_REF = 4.43361875` → `PHASE_INC ≈ 387,289,675,374` → 4.43 MHz subcarrier → TV colorburst PLL cannot lock → grayscale.

### Colorburst window
```cpp
int COLORBURST_START = (int)(3.7f * (CLK_VIDEO / CLK_REF));
int COLORBURST_END   = (int)(9.0f * (CLK_VIDEO / CLK_REF)) + COLORBURST_START;
```
These are pixel counts into the active line where the colorburst signal is gated on. For NTSC 15K: start≈13, end≈44.

### `yc_config` word
```cpp
uint16_t yc_config = ((pal || cfg.ntsc_mode) ? 4 : 0) | ((cfg.vga_mode_int == 3) ? 3 : 1);
// bits: [2]=pal_en, [1]=cvbs, [0]=yc_en
// 0x1 = NTSC S-Video, 0x3 = NTSC CVBS, 0x5 = PAL S-Video, 0x7 = PAL CVBS
```

---

## `UIO_SET_YC_PAR` SPI Protocol (command 0x41)

Seven 16-bit words sent after `spi_uio_cmd_cont(UIO_SET_YC_PAR)`:

| Word | Contents |
|------|----------|
| 0 | `{pal_en[2], cvbs[1], yc_en[0]}` |
| 1 | `PHASE_INC[15:0]` |
| 2 | `PHASE_INC[31:16]` |
| 3 | `PHASE_INC[39:32]` (top byte only) |
| 4 | `COLORBURST_RANGE[15:0]` (`{start[6:0], end[9:0]}` packed) |
| 5 | `COLORBURST_RANGE[16]` (MSB) |
| 6 | `subcarrier_enable` (1 for RGB+subcarrier mode, 0 otherwise) |

To disable YC entirely: `spi_uio_cmd8(UIO_SET_YC_PAR, 0)` (sends word 0 = 0).

---

## Native TV Modes

Defined in `tvmodes[]` in `video.cpp`:

| Mode | hact | hfp | hs | hbp | vact | vfp | vs | vbp | Fpix (MHz) | fps |
|------|------|-----|----|-----|------|-----|----|-----|------------|-----|
| NTSC 15K | 640 | 30 | 60 | 70 | 240 | 4 | 4 | 14 | 12.587 | 60.05 |
| PAL 15K | 640 | 30 | 60 | 70 | 288 | 4 | 4 | 14 | 12.587 | 50.43 |

htotal=800 for both. vtotal: NTSC=262, PAL=312.

`pal` determination: `fps < 55` — NTSC gives ~60 Hz → `pal=0`, PAL gives ~50 Hz → `pal=1`. This works correctly when `vtime` is live; it fails when `vtime=0` (game not started), producing `pal=1` regardless of mode.

---

## Why Grayscale (Not Black) on Unencoeded S-Video

S-Video has two pins:
- **Y (luma):** luminance + sync. Plain RGB-to-luma conversion is sufficient for the TV to sync and display brightness.
- **C (chroma):** QAM-modulated colorburst. Generated only by `yc_out`/`yc_out_fb`. Absent when encoder is disconnected or PHASE_INC is wrong.

Pipeline B plain RGB output contains luma information → TV syncs and shows brightness correctly → grayscale. `/dev/fb0` can contain full color data while the CRT shows gray because the chroma path is broken downstream.

---

## `yc_key` and `yc.txt` Override Mechanism

`set_yc_mode()` builds a lookup key: `{core_name}_{fps}{interlace_flag}{ntsc_suffix}` (e.g. `3SX_0.0` before game starts, `MENU_59.8` for the menu). This key is checked against entries in `yc.txt` (on the SD card) and a yc_modes array loaded from it. A matching entry overrides `PHASE_INC`. Used for per-core manual tuning corrections.

Caution: when `vtime=0`, the key is always `{core}_0.0`, which may unintentionally match a manual override entry.

---

## Call Sequence in 3SX Launch Path

```
threesx_core_context_init()        ← early startup
  → cfg_parse()
  → video_init()
      → set_default_tv_video_mode()
      → video_set_mode(&v_def, 0)  ← sets v_cur.Fpix = 12.587

... OSD mode selection, game launch picked ...

threesx_wrapper.cpp ~line 1068:
  disable_wrapper_osd()
  video_fb_clear(0)
  set_vga_fb(1)                    ← vga_fb=1 in HPS and FPGA
  video_chvt(runtime_vt)
  video_fb_enable(1)
  video_refresh_yc_mode()          ← set_yc_mode() called here
                                      current_video_info.vtime=0 at this point
  fork() + execve()                ← game starts, vtime populated after this
```

`video_refresh_yc_mode()` is a thin wrapper that just calls `set_yc_mode()`. The function is also called on mode changes and OSD events, but in the 3SX launch path the critical call is at line ~1075 before the game process starts.

---

## Confirmed Working Configuration (NTSC S-Video, 3SX)

After all three fixes (RTL `yc_out_fb`, `fb_native_analog_auto`, `vtime=0` PAL correction):

```
clock_source    = output-fb-auto
CLK_VIDEO       = 12.587000 MHz
CLK_REF         = 3.579545 MHz
phase_inc       ≈ 312,741,000,000
colorburst_start = 13
colorburst_end   = 44
yc_config       = 0x1  (pal_en=0, cvbs=0, yc_en=1)
subcarrier_enable = 0
```

Real CRT result: **full color on S-Video**.
