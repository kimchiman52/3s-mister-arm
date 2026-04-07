#!/usr/bin/env python3
"""
Exhaustive PLL search grouped by CE_PIXEL value.
Shows best results for each CE divider so we can evaluate architectural trade-offs.
"""

from dataclasses import dataclass

TARGET_HFREQ = 15_734.264
TARGET_VFREQ = 59.59949
FREF = 50_000_000
VCO_MIN = 600_000_000
VCO_MAX = 1_300_000_000
H_ACTIVE = 384
CURRENT_ACTIVE_TIME_US = 384 / 7_823_129 * 1e6

N_MAX = 512
M_MAX = 512
C_MAX = 512
H_TOTAL_RANGE = range(488, 520)
V_TOTAL_RANGE = range(260, 270)
CE_PIXELS = [1, 2, 4, 8]

HFREQ_TOLERANCE = 30
MAX_ACTIVE_TIME_CHANGE = 2.0
MAX_VFREQ_ERROR = 0.1  # relaxed to see what CE=4 can do


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


def search(ce_pixel):
    results = []
    for v_total in V_TOTAL_RANGE:
        for h_total in H_TOTAL_RANGE:
            target_pix = TARGET_HFREQ * h_total
            active_time_us = H_ACTIVE / target_pix * 1e6
            change_pct = abs(active_time_us - CURRENT_ACTIVE_TIME_US) / CURRENT_ACTIVE_TIME_US * 100
            if change_pct > MAX_ACTIVE_TIME_CHANGE:
                continue

            target_clk = target_pix * ce_pixel
            target_ratio = target_clk / FREF

            for m in range(1, M_MAX + 1):
                nc_target = target_ratio * m
                n_min = max(1, int(12 * m))
                n_max = min(N_MAX, int(26 * m))
                if n_min > N_MAX:
                    break

                for n in range(n_min, n_max + 1):
                    c_exact = n / nc_target
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
    return results


def print_table(results, title, count=15):
    # Sort by |V-freq error|, then |H-freq error|
    results.sort(key=lambda r: (abs(r.vfreq_error), abs(r.hfreq_error)))

    # Deduplicate
    seen = set()
    unique = []
    for r in results:
        key = (round(r.vfreq_error, 6), round(r.hfreq_error, 2))
        if key not in seen:
            seen.add(key)
            unique.append(r)

    print(f"\n{'='*140}")
    print(f"{title}  ({len(results)} total, {len(unique)} unique)")
    print(f"{'='*140}")
    print(f"{'#':>3} {'N':>4} {'M':>4} {'C':>4} {'CE':>3} {'H_TOT':>5} {'V_TOT':>5} "
          f"{'VCO MHz':>9} {'CLK_V MHz':>10} {'Pix MHz':>9} {'H-freq':>10} {'H err':>7} "
          f"{'V-freq':>12} {'V err Hz':>10} {'Stale/hr':>9} {'Δ act%':>7}")
    print("-" * 140)

    for i, r in enumerate(unique[:count]):
        stale_per_hr = abs(r.vfreq_error) * 3600 if r.vfreq_error != 0 else 0
        print(f"{i+1:3d} {r.n:4d} {r.m:4d} {r.c:4d} {r.ce_pixel:3d} {r.h_total:5d} {r.v_total:5d} "
              f"{r.vco_mhz:9.3f} {r.clk_video_mhz:10.4f} {r.pixel_clock_hz/1e6:9.6f} {r.hfreq:10.3f} {r.hfreq_error:+7.1f} "
              f"{r.vfreq:12.6f} {r.vfreq_error:+10.6f} {stale_per_hr:9.1f} {r.active_time_change_pct:+7.2f}")


print(f"Current: pixel_clock=7,823,129 Hz, H-freq=15,615 Hz, V-freq=59.5993 Hz, active={CURRENT_ACTIVE_TIME_US:.2f} μs")
print(f"Target:  H-freq=15,734 Hz (NTSC), V-freq=59.59949 Hz (CPS3)")

for ce in CE_PIXELS:
    results = search(ce)
    if results:
        print_table(results, f"CE_PIXEL = ÷{ce}  (CLK_VIDEO = {ce}× pixel clock)")
    else:
        print(f"\n{'='*140}")
        print(f"CE_PIXEL = ÷{ce}: NO RESULTS within constraints")
        print(f"{'='*140}")

# Also show the practical "sweet spot" — good enough for both, Quartus-friendly (small M,N)
print(f"\n{'='*140}")
print("PRACTICAL PICKS: V-freq error < 0.005 Hz, VCO margin > 50 MHz from edges, N ≤ 500, M ≤ 50")
print(f"{'='*140}")

all_results = []
for ce in CE_PIXELS:
    all_results.extend(search(ce))

practical = [r for r in all_results
             if abs(r.vfreq_error) < 0.005
             and r.vco_mhz > 650 and r.vco_mhz < 1250
             and r.n <= 500 and r.m <= 50]
practical.sort(key=lambda r: (abs(r.vfreq_error), abs(r.hfreq_error)))

seen = set()
unique_p = []
for r in practical:
    key = (r.n, r.m, r.c, r.ce_pixel, r.h_total, r.v_total)
    if key not in seen:
        seen.add(key)
        unique_p.append(r)

print(f"\n{len(unique_p)} candidates")
print(f"{'#':>3} {'N':>4} {'M':>4} {'C':>4} {'CE':>3} {'H_TOT':>5} {'V_TOT':>5} "
      f"{'VCO MHz':>9} {'CLK_V MHz':>10} {'Pix MHz':>9} {'H-freq':>10} {'H err':>7} "
      f"{'V-freq':>12} {'V err Hz':>10} {'Stale/hr':>9} {'Δ act%':>7}")
print("-" * 140)

for i, r in enumerate(unique_p[:20]):
    stale_per_hr = abs(r.vfreq_error) * 3600 if r.vfreq_error != 0 else 0
    print(f"{i+1:3d} {r.n:4d} {r.m:4d} {r.c:4d} {r.ce_pixel:3d} {r.h_total:5d} {r.v_total:5d} "
          f"{r.vco_mhz:9.3f} {r.clk_video_mhz:10.4f} {r.pixel_clock_hz/1e6:9.6f} {r.hfreq:10.3f} {r.hfreq_error:+7.1f} "
          f"{r.vfreq:12.6f} {r.vfreq_error:+10.6f} {stale_per_hr:9.1f} {r.active_time_change_pct:+7.2f}")
