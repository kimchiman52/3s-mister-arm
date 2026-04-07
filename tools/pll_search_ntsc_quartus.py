#!/usr/bin/env python3
"""
NTSC PLL search constrained to Quartus-feasible multiplier values.

Quartus 17 Lite empirical limits:
  - M=92 (multiplier): WORKS (current config)
  - M=171: FAILS ("illegal value")
  - M=373: FAILS (documented)

Conservative search: M_altera (our N) ≤ 120.
Also try specifying frequency with fewer decimal places (Quartus may round).
"""

from dataclasses import dataclass
from fractions import Fraction

TARGET_HFREQ = Fraction(9_000_000, 572)  # exact NTSC: 2250000/143
TARGET_VFREQ_F = 59.59949
FREF = 50_000_000
VCO_MIN = 600_000_000
VCO_MAX = 1_300_000_000
H_ACTIVE = 384
CURRENT_PIX = Fraction(50_000_000 * 92, 7 * 21 * 4)
CURRENT_ACTIVE_US = float(Fraction(H_ACTIVE, CURRENT_PIX)) * 1e6

# Quartus-safe multiplier range
N_MAX = 120  # Altera M (multiplier) — stay well under 171
M_MAX = 50   # Altera N (input divider)
C_MAX = 512
H_TOTAL_RANGE = range(488, 520)
V_TOTAL_RANGE = range(260, 270)
CE_PIXELS = [1, 2, 4]

HFREQ_TOLERANCE = 30
MAX_ACTIVE_TIME_CHANGE = 2.0
MAX_VFREQ_ERROR = 0.05


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
    quartus_freq_str: str


results = []

for ce_pixel in CE_PIXELS:
    for v_total in V_TOTAL_RANGE:
        for h_total in H_TOTAL_RANGE:
            target_pix = float(TARGET_HFREQ) * h_total
            active_time_us = H_ACTIVE / target_pix * 1e6
            change_pct = abs(active_time_us - CURRENT_ACTIVE_US) / CURRENT_ACTIVE_US * 100
            if change_pct > MAX_ACTIVE_TIME_CHANGE:
                continue

            for m in range(1, M_MAX + 1):
                n_min = max(1, int(12 * m))
                n_max = min(N_MAX, int(26 * m))
                if n_min > N_MAX:
                    break

                for n in range(n_min, n_max + 1):
                    vco = FREF * n / m
                    if vco < VCO_MIN or vco > VCO_MAX:
                        continue

                    # Find C values that put pixel clock near target
                    target_clk = float(TARGET_HFREQ) * h_total * ce_pixel
                    c_exact = vco / target_clk
                    for c in [int(c_exact), int(c_exact) + 1]:
                        if c < 1 or c > C_MAX:
                            continue

                        clk_video = vco / c
                        pixel_clock = clk_video / ce_pixel
                        hfreq = pixel_clock / h_total
                        vfreq = hfreq / v_total

                        hfreq_error = hfreq - float(TARGET_HFREQ)
                        vfreq_error = vfreq - TARGET_VFREQ_F

                        if abs(hfreq_error) > HFREQ_TOLERANCE:
                            continue
                        if abs(vfreq_error) > MAX_VFREQ_ERROR:
                            continue

                        active_time = H_ACTIVE / pixel_clock * 1e6
                        at_change = (active_time - CURRENT_ACTIVE_US) / CURRENT_ACTIVE_US * 100

                        # Quartus frequency string (6 decimal places)
                        freq_str = f"{clk_video/1e6:.6f}"

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
                            quartus_freq_str=freq_str,
                        ))

results.sort(key=lambda r: (abs(r.vfreq_error), abs(r.hfreq_error)))

seen = set()
unique = []
for r in results:
    key = (round(r.vfreq_error, 6), round(r.hfreq_error, 2))
    if key not in seen:
        seen.add(key)
        unique.append(r)

print(f"Total: {len(results)}, Unique: {len(unique)}")
print(f"Multiplier (Altera M) capped at {N_MAX}")
print(f"Current: pix=7,823,129 Hz, H-freq=15,615 Hz, V-freq=59.5993 Hz, active={CURRENT_ACTIVE_US:.2f} us")
print()

hdr = (f"{'#':>3} {'aM':>4} {'aN':>4} {'C':>4} {'CE':>3} {'H_TOT':>5} {'V_TOT':>5} "
       f"{'VCO MHz':>9} {'Quartus freq':>14} {'Pix MHz':>9} {'H-freq':>10} {'H err':>7} "
       f"{'V-freq':>12} {'V err Hz':>10} {'Stale/hr':>9} {'Δ act%':>7}")
print(hdr)
print("-" * len(hdr))

for i, r in enumerate(unique[:30]):
    stale_hr = abs(r.vfreq_error) * 3600
    print(f"{i+1:3d} {r.n:4d} {r.m:4d} {r.c:4d} {r.ce_pixel:3d} {r.h_total:5d} {r.v_total:5d} "
          f"{r.vco_mhz:9.3f} {r.quartus_freq_str:>14} {r.pixel_clock_hz/1e6:9.6f} {r.hfreq:10.3f} {r.hfreq_error:+7.1f} "
          f"{r.vfreq:12.6f} {r.vfreq_error:+10.6f} {stale_hr:9.1f} {r.active_time_change_pct:+7.2f}")

# Highlight CE=4 options specifically
print("\n\nCE_PIXEL=4 only (same architecture as current):")
ce4 = [r for r in unique if r.ce_pixel == 4]
print(f"{len(ce4)} candidates")
print(hdr)
print("-" * len(hdr))
for i, r in enumerate(ce4[:20]):
    stale_hr = abs(r.vfreq_error) * 3600
    print(f"{i+1:3d} {r.n:4d} {r.m:4d} {r.c:4d} {r.ce_pixel:3d} {r.h_total:5d} {r.v_total:5d} "
          f"{r.vco_mhz:9.3f} {r.quartus_freq_str:>14} {r.pixel_clock_hz/1e6:9.6f} {r.hfreq:10.3f} {r.hfreq_error:+7.1f} "
          f"{r.vfreq:12.6f} {r.vfreq_error:+10.6f} {stale_hr:9.1f} {r.active_time_change_pct:+7.2f}")
