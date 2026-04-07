#!/usr/bin/env python3
"""
Exhaustive PLL search for NTSC-compatible native video timing.

Constraints:
  - Cyclone V integer-N PLL: VCO = 50 MHz × N / M, output = VCO / C
  - VCO range: 600–1300 MHz
  - CE_PIXEL: {1, 2, 4, 8}
  - pixel_clock = 50e6 * N / (M * C * CE_PIXEL)
  - H_TOTAL, V_TOTAL: adjustable integers
  - H-freq = pixel_clock / H_TOTAL must be ≈ 15,734 Hz (NTSC, within ±30 Hz)
  - V-freq = H-freq / V_TOTAL must be ≈ 59.59949 Hz (CPS3 target)
  - Active area time = 384 / pixel_clock should be close to current 49.08 μs (< 2% change)

Current config for reference:
  PLL: 50 MHz × 92/7, /21 = 31.2925 MHz CLK_VIDEO
  CE_PIXEL = 4, pixel_clock = 7.8231 MHz
  H_TOTAL = 501, V_TOTAL = 262
  H-freq = 15,615 Hz, V-freq = 59.5993 Hz
"""

import sys
from dataclasses import dataclass

TARGET_HFREQ = 15_734.264  # NTSC standard H-freq
TARGET_VFREQ = 59.59949    # CPS3 target
FREF = 50_000_000          # 50 MHz reference
VCO_MIN = 600_000_000
VCO_MAX = 1_300_000_000
H_ACTIVE = 384
CURRENT_ACTIVE_TIME_US = 384 / 7_823_129 * 1e6  # 49.08 μs

# Search ranges
N_MAX = 512
M_MAX = 512
C_MAX = 512
H_TOTAL_RANGE = range(490, 520)
V_TOTAL_RANGE = range(260, 270)
CE_PIXELS = [1, 2, 4, 8]

# Filter thresholds
HFREQ_TOLERANCE = 30       # Hz
MAX_ACTIVE_TIME_CHANGE = 2  # percent
MAX_VFREQ_ERROR = 0.05     # Hz


@dataclass
class Result:
    n: int
    m: int
    c: int
    ce_pixel: int
    h_total: int
    v_total: int
    vco_mhz: float
    clk_video_mhz: float
    pixel_clock_hz: float
    hfreq: float
    vfreq: float
    hfreq_error: float
    vfreq_error: float
    active_time_us: float
    active_time_change_pct: float


results = []

for ce_pixel in CE_PIXELS:
    for v_total in V_TOTAL_RANGE:
        for h_total in H_TOTAL_RANGE:
            # Target pixel clock for this H_TOTAL to hit NTSC H-freq
            # We want pixel_clock / h_total ≈ 15734
            # So pixel_clock ≈ 15734 * h_total
            target_pix = TARGET_HFREQ * h_total

            # Check active time constraint early
            active_time_us = H_ACTIVE / target_pix * 1e6
            change_pct = abs(active_time_us - CURRENT_ACTIVE_TIME_US) / CURRENT_ACTIVE_TIME_US * 100
            if change_pct > MAX_ACTIVE_TIME_CHANGE:
                continue

            # target CLK_VIDEO
            target_clk = target_pix * ce_pixel

            # target VCO / C = target_clk, so for each M, N:
            # VCO = FREF * N / M
            # output = VCO / C = FREF * N / (M * C)
            # pixel = output / ce_pixel = FREF * N / (M * C * ce_pixel)
            #
            # We want FREF * N / (M * C * ce_pixel) as close to target_pix as possible
            # Rearranging: N / (M * C) = target_pix * ce_pixel / FREF = target_clk / FREF

            target_ratio = target_clk / FREF  # N / (M*C) we want

            for m in range(1, M_MAX + 1):
                # For this M, we need N / C ≈ target_ratio * M
                nc_target = target_ratio * m  # = N / C target

                # VCO = 50e6 * N / M, must be in [600e6, 1300e6]
                # So N must be in [600e6 * M / 50e6, 1300e6 * M / 50e6]
                #                = [12 * M, 26 * M]
                n_min = max(1, int(12 * m))
                n_max = min(N_MAX, int(26 * m))
                if n_min > N_MAX:
                    break

                for n in range(n_min, n_max + 1):
                    # C = N / nc_target
                    c_exact = n / nc_target
                    # Try floor and ceil
                    for c in [int(c_exact), int(c_exact) + 1]:
                        if c < 1 or c > C_MAX:
                            continue

                        vco = FREF * n / m
                        if vco < VCO_MIN or vco > VCO_MAX:
                            continue

                        clk_video = vco / c
                        pixel_clock = clk_video / ce_pixel
                        hfreq = pixel_clock / h_total
                        vfreq = hfreq / v_total

                        hfreq_error = hfreq - TARGET_HFREQ
                        vfreq_error = vfreq - TARGET_VFREQ

                        if abs(hfreq_error) > HFREQ_TOLERANCE:
                            continue
                        if abs(vfreq_error) > MAX_VFREQ_ERROR:
                            continue

                        active_time = H_ACTIVE / pixel_clock * 1e6
                        at_change = (active_time - CURRENT_ACTIVE_TIME_US) / CURRENT_ACTIVE_TIME_US * 100

                        results.append(Result(
                            n=n, m=m, c=c, ce_pixel=ce_pixel,
                            h_total=h_total, v_total=v_total,
                            vco_mhz=vco / 1e6,
                            clk_video_mhz=clk_video / 1e6,
                            pixel_clock_hz=pixel_clock,
                            hfreq=hfreq, vfreq=vfreq,
                            hfreq_error=hfreq_error,
                            vfreq_error=vfreq_error,
                            active_time_us=active_time,
                            active_time_change_pct=at_change,
                        ))

# Sort by V-freq error (primary), then H-freq error (secondary)
results.sort(key=lambda r: (abs(r.vfreq_error), abs(r.hfreq_error)))

# Deduplicate by (vfreq_error rounded to 6 decimals, hfreq_error rounded to 2)
seen = set()
unique = []
for r in results:
    key = (round(r.vfreq_error, 6), round(r.hfreq_error, 2))
    if key not in seen:
        seen.add(key)
        unique.append(r)

print(f"Total candidates found: {len(results)}")
print(f"Unique (by freq): {len(unique)}")
print()
print(f"Current config: pixel_clock=7,823,129 Hz, H-freq=15,615 Hz, V-freq=59.5993 Hz, active={CURRENT_ACTIVE_TIME_US:.2f} μs")
print()

# Show top 30
print("Top 30 by V-freq accuracy (within ±30 Hz H-freq, <2% active time change):")
print()
print(f"{'#':>3} {'N':>4} {'M':>4} {'C':>4} {'CE':>3} {'H_TOT':>5} {'V_TOT':>5} "
      f"{'VCO MHz':>9} {'Pix MHz':>9} {'H-freq':>10} {'H err':>7} "
      f"{'V-freq':>12} {'V err Hz':>10} {'Act μs':>7} {'Δ act%':>7}")
print("-" * 140)

for i, r in enumerate(unique[:30]):
    print(f"{i+1:3d} {r.n:4d} {r.m:4d} {r.c:4d} {r.ce_pixel:3d} {r.h_total:5d} {r.v_total:5d} "
          f"{r.vco_mhz:9.3f} {r.pixel_clock_hz/1e6:9.6f} {r.hfreq:10.3f} {r.hfreq_error:+7.1f} "
          f"{r.vfreq:12.6f} {r.vfreq_error:+10.6f} {r.active_time_us:7.3f} {r.active_time_change_pct:+7.2f}")

# Also show best by combined metric (weighted V-freq + H-freq)
print()
print("Top 10 by combined metric (|V_err|*1000 + |H_err|):")
results.sort(key=lambda r: abs(r.vfreq_error) * 1000 + abs(r.hfreq_error))
seen2 = set()
unique2 = []
for r in results:
    key = (round(r.vfreq_error, 6), round(r.hfreq_error, 2))
    if key not in seen2:
        seen2.add(key)
        unique2.append(r)

print()
print(f"{'#':>3} {'N':>4} {'M':>4} {'C':>4} {'CE':>3} {'H_TOT':>5} {'V_TOT':>5} "
      f"{'VCO MHz':>9} {'Pix MHz':>9} {'H-freq':>10} {'H err':>7} "
      f"{'V-freq':>12} {'V err Hz':>10} {'Act μs':>7} {'Δ act%':>7}")
print("-" * 140)

for i, r in enumerate(unique2[:10]):
    print(f"{i+1:3d} {r.n:4d} {r.m:4d} {r.c:4d} {r.ce_pixel:3d} {r.h_total:5d} {r.v_total:5d} "
          f"{r.vco_mhz:9.3f} {r.pixel_clock_hz/1e6:9.6f} {r.hfreq:10.3f} {r.hfreq_error:+7.1f} "
          f"{r.vfreq:12.6f} {r.vfreq_error:+10.6f} {r.active_time_us:7.3f} {r.active_time_change_pct:+7.2f}")
