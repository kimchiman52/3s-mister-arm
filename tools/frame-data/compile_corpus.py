#!/usr/bin/env python3
"""Compile a frame-data corpus YAML into a script.fdi input script plus an
expected.json oracle for tools/frame-data/check_frame_data.py.

See docs/plan-frame-data-harness.md section 1.7 (Step H5) for the spec this
implements. The .fdi format itself is defined and parsed by
src/test/input_script.c - this compiler must emit exactly what that parser
accepts (W/L/P/G/Q directives, `#` comments, lines <= 255 chars).

Usage: python3 compile_corpus.py <corpus.yaml> <output-dir>
Writes <output-dir>/script.fdi, <output-dir>/expected.json, and
<output-dir>/meta.json (Phase 6 Step 3: {"p1_character": <id>}, read by
run.sh to pass --test-p1-character through to the game invocation).
meta.json also carries optional `p1_super_art` / `sa_gauge` / `p1_super_full`
keys (EX/Supers program Step 1 procedure item 4; `super_full:` added by task
#108) - present ONLY when the corpus sets the corresponding top-level key, so
meta.json (and script.fdi/expected.json, untouched by these features) stay
byte-identical for every corpus that doesn't use them.

meta.json ALWAYS carries `balance` ("ps2" | "arcade", task #108): which engine
the corpus is measured against. It is unconditional precisely because the
engine under test used to be implicit - run.sh passed --test-enable and
arcade_balance.c inferred PS2 from it - so every 94-corpus "green" run
measured the port while the shipping device auto-selects arcade. Every RUNDIR
now records in writing which engine it ran.
"""

import json
import re
import sys
from pathlib import Path

import yaml

REPO_ROOT = Path(__file__).resolve().parents[2]
ORACLE_DIR = REPO_ROOT / "docs" / "arcade-frame-data"
Q_JSON_PATH = ORACLE_DIR / "q.json"
DEFAULT_ORACLE = "q.json"

# Phase 6 Step 3: per-corpus `character:`/`oracle:` keys (see plan
# docs/plan-frame-data-harness.md Phase 6 sec 1.5/3). Character ids match the
# non-CPS3 `Character` enum (src/constants.h:38-59) - the enum the host build
# actually compiles (CPS3 is a separate, differently-numbered layout gated
# `#if CPS3` in the same file and is NOT what `build/host` uses). Kept as the
# full 20-character table (not just the plan's minimum q/ryu/hugo/ken) since
# it costs nothing extra and every future per-character corpus needs an entry
# here anyway.
CHARACTER_IDS = {
    "gill": 0,
    "alex": 1,
    "ryu": 2,
    "yun": 3,
    "dudley": 4,
    "necro": 5,
    "hugo": 6,
    "ibuki": 7,
    "elena": 8,
    "oro": 9,
    "yang": 10,
    "ken": 11,
    "sean": 12,
    "urien": 13,
    "akuma": 14,
    "chunli": 15,
    "makoto": 16,
    "q": 17,
    "twelve": 18,
    "remy": 19,
}
DEFAULT_CHARACTER = "q"

# --- Tunables -----------------------------------------------------------
# Motion/press timings are deliberately module-level constants (not exposed
# in the corpus YAML) so every entry stays comparable. Plan section 1.10
# flags these as likely needing iteration once real cmd_data.c timing
# windows are exercised in Step H6 - if a motion doesn't come out, check
# these first.
MOTION_STEP_FRAMES = 2       # frames held per lever direction in a motion macro
PRESS_HOLD_FRAMES = 2        # frames a plain `press` chord is held (plan: "2 frames")
PRESS_RELEASE_FRAMES = 1     # neutral frames after a press/motion, so the next
                              # command's rising edge (sw_new) is genuinely fresh
PRESS1F_LEAD_FRAMES = 1      # frames the first button of `press1f A+B` is held alone
                              # before B joins, for the same-frame vs. 1-frame-apart
                              # UOH variants (plan section 1.7 / synthesis Sec13.7.2)
CHARGE_SNAP_FRAMES = 2       # frames the final forward/up + button of a charge move
                              # is held once the charge releases
DEFAULT_INTER_ENTRY_WAIT = 120  # frames of neutral between entries (plan section 1.10);
                                  # override per-corpus via a top-level `inter_entry_wait` key

# setup.dist teleport geometry. Default placement mode ("anchored", `setup.dist_mode`
# absent or "anchored"): P1 is always anchored at NUMERIC_BASE_X and P2 sits `gap`
# pixels to its right - this is the "keep it simple" formula from plan section 1.7
# ("P1 at center-gap/2 etc... e.g. P 424 (424+gap)"), verified empirically 2026-07-06:
#   - P 424 480 (gap=56)  -> atk_x=424 def_x=480 dist=56  -> close-range Jab (S=4 A=4 R=6)
#   - P 424 599 (gap=175) -> atk_x=425 def_x=599 dist=174 -> matches the untouched default
#     training spawn (atk_x=425 def_x=600 dist=175) to within 1px of engine idle drift.
# CAVEAT, root-caused as far as it needs to be (EX/Supers program, Ruling 1,
# <sp>/exsuper/authoring-policy.md - the exact engine clamp site itself was searched
# for and not located; its behavior is characterized empirically only, twice now):
# teleporting to a gap much larger than ~252px measured-achieved gets silently reduced
# by a position/stage-bound clamp that fires the same frame, before the trace snapshot.
# The falsified claim in the previous revision of this comment ("keep dist <= 300 is
# safe") is corrected here: 9 corpus entries across the shipped 19-corpus suite use
# `dist: 300` under ANCHORED placement, and 2b's own measurement showed those actually
# run at ~252px, not 300 - goldens are frozen against that achieved ~252px spacing
# (P1 pinned at NUMERIC_BASE_X=424 sits close to the left stage bound, which eats the
# rest of the requested gap) so DIST_MAX stays 300 and nothing here moves any existing
# corpus/golden. Root-cause split (measured, not guessed): part harness geometry (P1's
# anchor position), part genuine engine position bound - the same engine the arcade
# runs, not a port defect.
#
# `setup.dist_mode: centered` (opt-in, EX/Supers program tooling micro-batch) places
# the pair symmetrically about CENTER_X instead of anchoring P1 at NUMERIC_BASE_X,
# recovering the ~70-80px of shortfall that was pure anchor-position artifact. Absent
# key => today's ANCHORED behavior, byte-identical script.fdi/expected.json/meta.json
# for every one of the 20 corpora that predate this key (verified via the compile-diff
# gate - see the EX/Supers tooling-batch commit message). Measured this session
# (single probe run, requesting a symmetric `dist: 400` around CENTER_X=500 - i.e.
# `P 300 700` - and reading P1.x/P2.x off the MOVE_START annotation, the first tick
# that actually lands in the trace): achieved separation 328px (matches the
# previously-recorded E-6 observation of "~320-332" for a symmetric request almost
# exactly, confirming CENTER_X=500 sits in the same wide, engine-native window and
# is not itself an additional bottleneck) - CENTERED_DIST_MAX is set to 310, 18px
# inside that measured ceiling, so every enforced `dist_mode: centered` request is
# guaranteed reachable rather than sitting exactly on the clamp's edge (re-verified
# unclamped at dist=310 during the Ryu EX pilot's own whiff legs - see corpus-ryu-
# ex.yaml).
NUMERIC_BASE_X = 424
CLOSE_DIST = 56
FAR_DIST = 175
DIST_MAX = 300
CENTER_X = 500
CENTERED_DIST_MAX = 310
DIST_MODES = ("anchored", "centered")

BTN_BITS = {
    "LP": 0x0010,
    "MP": 0x0020,
    "HP": 0x0040,
    "LK": 0x0100,
    "MK": 0x0200,
    "HK": 0x0400,
}

# SWK_* bit layout (include/sf33rd/AcrSDK/common/pad.h), confirmed against
# src/test/replay_game.c:12-26's raw-format-to-SWK translation table.
DIR_BITS = {
    "UP": 0x1,
    "DOWN": 0x2,
    "LEFT": 0x4,
    "RIGHT": 0x8,
}

# P1 is always the attacker on the left, facing right - the harness never
# switches sides (plan section 1.7: "Player never switches sides in harness
# runs"), so forward/back are fixed regardless of corpus content.
FORWARD = DIR_BITS["RIGHT"]
BACK = DIR_BITS["LEFT"]
DOWN = DIR_BITS["DOWN"]
UP = DIR_BITS["UP"]

GUARD_MODES = ("none", "stand", "crouch")
# NONE is a negative-control outcome (plan section 1.8 item 5): the window must
# finalize with ZERO FINAL lines. It carries no S/A/R/adv expectations - see
# validate_entry_shape's NONE-outcome check.
OUTCOMES = ("HIT", "BLOCK", "WHIFF", "NONE")
# "kd" (knockdown flag, Phase 4 item 3) is int 0/1 only - deliberately NOT
# resolvable via from-qjson (see the kd-specific branch in compile_entry):
# q.json's non-numeric "Hit_advantage": "D" is not auto-mapped to kd=1, a
# KD expectation is explicit corpus authorship.
EXPECT_NUMERIC_FIELDS = ("S", "A", "R", "adv", "kd")

# Labels become both the .fdi `L <label>` directive text and a dict key/CLI-
# visible identifier in expected.json and the checker's table, so they're kept
# to a conservative, unambiguous character set rather than allowing everything
# the C parser happens to tolerate. `#` is rejected because
# src/test/input_script.c:167-171 treats the first '#' anywhere on a line as
# the start of a comment - a label containing '#' would be silently truncated
# by the C parser instead of failing loudly. Max length matches the C side's
# hard limit (input_script.c:20 INPUT_SCRIPT_LABEL_MAX_LEN=64, checked with
# strlen(label) >= 64, so 63 chars is the longest label the parser accepts).
LABEL_RE = re.compile(r"^[A-Za-z0-9._-]+$")
LABEL_MAX_LEN = 63

QJSON_FIELD_FOR = {"S": "Startup", "A": "Hit", "R": "Recovery"}


class CompileError(Exception):
    """Corpus/qjson error attributable to corpus authoring, reported to the
    user without a Python traceback (main() catches this and exits 1)."""


def bits_for_token(token, context):
    token = token.strip().upper()
    if token in BTN_BITS:
        return BTN_BITS[token]
    if token in DIR_BITS:
        return DIR_BITS[token]
    raise CompileError(f"{context}: unknown button/direction token '{token}'")


def parse_chord(chord_str, context):
    total = 0
    tokens = [t for t in chord_str.split("+") if t.strip()]
    if not tokens:
        raise CompileError(f"{context}: empty chord")
    for tok in tokens:
        total |= bits_for_token(tok, context)
    return total


def parse_int_token(tok, context):
    try:
        return int(tok)
    except ValueError:
        raise CompileError(f"{context}: expected an integer, got '{tok}'")


def parse_positive_int_token(tok, context):
    frames = parse_int_token(tok, context)
    if frames <= 0:
        raise CompileError(f"{context}: frame count must be positive, got {frames}")
    return frames


# --- Input macro layer ---------------------------------------------------
# Each command function returns a list of (p1_word, frames) pairs; the
# caller flattens these into `W <word> 0000 <frames>` lines (p2 is always
# neutral - the dummy is driven by the G directive/engine AI, never scripted
# input in this harness).

def cmd_press(rest, context):
    chord = parse_chord(rest, context)
    return [(chord, PRESS_HOLD_FRAMES), (0, PRESS_RELEASE_FRAMES)]


def cmd_press1f(rest, context):
    tokens = [t.strip() for t in rest.split("+") if t.strip()]
    if len(tokens) < 2:
        raise CompileError(f"{context}: press1f requires at least two '+'-joined tokens (A+B)")
    lead = bits_for_token(tokens[0], context)
    joined = 0
    for tok in tokens[1:]:
        joined |= bits_for_token(tok, context)
    full = lead | joined
    return [(lead, PRESS1F_LEAD_FRAMES), (full, PRESS_HOLD_FRAMES), (0, PRESS_RELEASE_FRAMES)]


def cmd_hold(rest, context):
    parts = rest.split()
    if len(parts) != 2:
        raise CompileError(f"{context}: 'hold' needs exactly '<chord> <frames>', got '{rest}'")
    chord = parse_chord(parts[0], context)
    frames = parse_positive_int_token(parts[1], context)
    return [(chord, frames)]


def cmd_wait(rest, context):
    frames = parse_positive_int_token(rest.strip(), context)
    return [(0, frames)]


def _motion_steps(name, btn):
    if name == "qcf":
        seq, final = [DOWN, DOWN | FORWARD], FORWARD
    elif name == "qcb":
        seq, final = [DOWN, DOWN | BACK], BACK
    elif name == "hcb":
        seq, final = [FORWARD, FORWARD | DOWN, DOWN, DOWN | BACK], BACK
    elif name == "hcf":
        seq, final = [BACK, BACK | DOWN, DOWN, DOWN | FORWARD], FORWARD
    elif name == "2qcf":
        seq, final = [DOWN, DOWN | FORWARD, FORWARD, DOWN, DOWN | FORWARD], FORWARD
    elif name == "2qcb":
        # EX/Supers program, Step 2b scratch need (back-motion double-QCB
        # supers, e.g. Chun-Li Houyoku Sen): mirrors "2qcf" verbatim with
        # BACK in place of FORWARD. No existing corpus uses this token, so
        # this is purely additive - zero effect on the 19 committed corpora.
        seq, final = [DOWN, DOWN | BACK, BACK, DOWN, DOWN | BACK], BACK
    else:
        raise ValueError(f"unhandled motion name '{name}'")  # pragma: no cover - dispatch bug
    return [*seq, final | btn]


def cmd_motion(rest, context):
    tokens = rest.split()
    if not tokens:
        raise CompileError(f"{context}: 'motion' requires arguments")
    name = tokens[0].lower()

    if name in ("qcf", "qcb", "hcb", "hcf", "2qcf", "2qcb"):
        if len(tokens) != 2:
            raise CompileError(f"{context}: 'motion {name}' needs exactly one button, got '{rest}'")
        btn = bits_for_token(tokens[1], context)
        steps = _motion_steps(name, btn)
        out = [(w, MOTION_STEP_FRAMES) for w in steps]
        out.append((0, PRESS_RELEASE_FRAMES))
        return out

    if name == "charge-back":
        if len(tokens) != 4 or tokens[2] != "then-forward":
            raise CompileError(
                f"{context}: expected 'motion charge-back <N> then-forward <BTN>', got '{rest}'"
            )
        charge_frames = parse_positive_int_token(tokens[1], context)
        btn = bits_for_token(tokens[3], context)
        return [
            (BACK, charge_frames),
            (FORWARD | btn, CHARGE_SNAP_FRAMES),
            (0, PRESS_RELEASE_FRAMES),
        ]

    if name == "charge-down":
        if len(tokens) != 4 or tokens[2] != "then-up":
            raise CompileError(
                f"{context}: expected 'motion charge-down <N> then-up <BTN>', got '{rest}'"
            )
        charge_frames = parse_positive_int_token(tokens[1], context)
        btn = bits_for_token(tokens[3], context)
        return [
            (DOWN, charge_frames),
            (UP | btn, CHARGE_SNAP_FRAMES),
            (0, PRESS_RELEASE_FRAMES),
        ]

    raise CompileError(f"{context}: unknown motion '{name}'")


INPUT_COMMANDS = {
    "press": cmd_press,
    "press1f": cmd_press1f,
    "hold": cmd_hold,
    "wait": cmd_wait,
    "motion": cmd_motion,
}


def parse_input_commands(input_str, label):
    words = []
    for raw_cmd in input_str.split(";"):
        raw_cmd = raw_cmd.strip()
        if not raw_cmd:
            continue
        parts = raw_cmd.split(None, 1)
        verb = parts[0].lower()
        rest = parts[1] if len(parts) > 1 else ""
        context = f"entry '{label}', command '{raw_cmd}'"
        handler = INPUT_COMMANDS.get(verb)
        if handler is None:
            raise CompileError(f"{context}: unknown input command '{verb}'")
        words.extend(handler(rest, context))
    return words


# --- setup.{dist,dummy} ---------------------------------------------------

def resolve_setup_dist(entry):
    setup = entry.get("setup") or {}
    dist = setup.get("dist", "far")
    label = entry["label"]
    dist_mode = setup.get("dist_mode", "anchored")
    if dist_mode not in DIST_MODES:
        raise CompileError(
            f"entry '{label}': setup.dist_mode must be one of {DIST_MODES}, got {dist_mode!r}"
        )
    dist_max = CENTERED_DIST_MAX if dist_mode == "centered" else DIST_MAX
    if dist == "close":
        gap = CLOSE_DIST
    elif dist == "far":
        gap = FAR_DIST
    elif isinstance(dist, int) and not isinstance(dist, bool):
        if dist <= 0:
            raise CompileError(f"entry '{label}': setup.dist must be positive, got {dist}")
        if dist > dist_max:
            raise CompileError(
                f"entry '{label}': setup.dist {dist} exceeds {dist_max}px for "
                f"dist_mode={dist_mode!r} - the engine silently clamps larger requested "
                f"gaps to a shorter actual distance (see the CAVEAT comment above), so "
                f"expected.json would describe spacing that was never actually reached; "
                f"keep dist <= {dist_max} for this dist_mode"
            )
        gap = dist
    else:
        raise CompileError(
            f"entry '{label}': setup.dist must be 'close', 'far', or a positive int, got {dist!r}"
        )
    if dist_mode == "centered":
        p1_x = CENTER_X - gap // 2
        p2_x = p1_x + gap
    else:
        p1_x = NUMERIC_BASE_X
        p2_x = NUMERIC_BASE_X + gap
    assert p1_x < p2_x  # invariant: P1 stays left of P2 (harness never flips sides)
    return p1_x, p2_x


def resolve_setup_dummy(entry):
    setup = entry.get("setup") or {}
    dummy = setup.get("dummy", "none")
    label = entry["label"]
    if dummy not in GUARD_MODES:
        raise CompileError(
            f"entry '{label}': setup.dummy must be one of {GUARD_MODES}, got {dummy!r}"
        )
    return dummy


# --- arcade oracle (q.json / ryu.json / hugo.json / ...) -------------------
# Despite the name (kept for minimal diff against the pre-Step-3 Q-only
# harness), `load_qjson` is character-agnostic: it just reads whichever
# oracle JSON the corpus's top-level `oracle:` key resolved to (all files
# under docs/arcade-frame-data/ share the same Coccis77/thirdstrikedatabot
# per-move schema - see resolve_oracle_path/compile_corpus).

def load_qjson(oracle_path):
    try:
        with open(oracle_path, "r") as f:
            data = json.load(f)
    except OSError as e:
        raise CompileError(f"could not read oracle at {oracle_path}: {e}")
    by_name = {}
    for entry in data:
        by_name.setdefault(entry["Name"], []).append(entry)
    return by_name


def resolve_character(value, context):
    if isinstance(value, bool):
        raise CompileError(f"{context}: 'character' must be a name or int, got {value!r}")
    if isinstance(value, int):
        char_id = value
    elif isinstance(value, str):
        key = value.strip().lower()
        if key not in CHARACTER_IDS:
            raise CompileError(
                f"{context}: unknown character {value!r} (known names: "
                f"{', '.join(sorted(CHARACTER_IDS))})"
            )
        char_id = CHARACTER_IDS[key]
    else:
        raise CompileError(f"{context}: 'character' must be a name or int, got {value!r}")
    if char_id < 0 or char_id > 19:
        raise CompileError(
            f"{context}: character id {char_id} out of range 0..19 "
            f"(src/constants.h non-CPS3 Character enum, args.c --test-p1-character bound)"
        )
    return char_id


# EX/Supers program, Step 1 procedure item 4: optional top-level `super_art:`
# / `sa_gauge:` corpus keys, threaded to --test-p1-super-art / --test-training-
# sa-gauge (args.c bounds: super_art 0-2, sa_gauge 0-3 - see src/args.c). Both
# default to None (absent from the corpus) rather than a sentinel int, so
# meta.json only gains the corresponding key when a corpus actually asks for
# it - required for the byte-identity gate over the 19 existing corpora, none
# of which set either key.
def resolve_super_art(value, context):
    if value is None:
        return None
    if not isinstance(value, int) or isinstance(value, bool) or value not in (0, 1, 2):
        raise CompileError(f"{context}: 'super_art' must be an int 0-2, got {value!r}")
    return value


def resolve_sa_gauge(value, context):
    if value is None:
        return None
    if not isinstance(value, int) or isinstance(value, bool) or value not in (0, 1, 2, 3):
        raise CompileError(f"{context}: 'sa_gauge' must be an int 0-3, got {value!r}")
    return value


# Task #108: optional top-level `balance:` corpus key, threaded to the game's
# --test-balance. This is the corpus stating WHICH ENGINE its expectations were
# authored against - "ps2" (the port tables) or "arcade" (the shipping CPS3
# tables, ArcadeBalance_IsEnabled()). It defaults to "ps2", which is what all
# 94 pre-#108 corpora were measured under, so their goldens are unaffected.
#
# Unlike `super_art`/`sa_gauge`, this key is ALWAYS written into meta.json even
# when the corpus omits it. The whole point of #108 is that the engine under
# test stops being implicit: every RUNDIR now records, in writing, which one it
# ran, and run.sh always passes the flag rather than letting the binary decide.
BALANCE_VALUES = ("ps2", "arcade")
DEFAULT_BALANCE = "ps2"


def resolve_balance(value, context):
    if value is None:
        return DEFAULT_BALANCE
    if not isinstance(value, str) or value not in BALANCE_VALUES:
        raise CompileError(
            f"{context}: 'balance' must be one of {BALANCE_VALUES} (src/args.c --test-balance), "
            f"got {value!r}"
        )
    return value


# Task #108: optional top-level `super_full:` corpus key -> --test-p1-super-full.
#
# Why an ARCADE corpus needs this and `sa_gauge:` will not do: the training
# S.A.GAUGE menu options (`sa_gauge:`) are implemented by the PS2 half of
# player_mv_0000, inside `if (!ArcadeBalance_IsEnabled())`
# (src/sf33rd/Source/Game/engine/plmain.c:183-204) - the whole
# demo_set_sa_full / clear_super_arts_point switch is skipped under arcade
# balance, so an arcade corpus that asks for `sa_gauge: 3` gets NO meter at
# all. --test-p1-super-full goes through test_runner.c's own
# tr_spgauge_cont_init2 (src/test/test_runner.c:993-1009), which is not gated
# on balance, so it is the meter lever that works on both engines.
def resolve_super_full(value, context):
    if value is None:
        return None
    if not isinstance(value, bool):
        raise CompileError(f"{context}: 'super_full' must be a bool, got {value!r}")
    return value


def resolve_oracle_path(value, context):
    if not isinstance(value, str) or not value.strip():
        raise CompileError(f"{context}: 'oracle' must be a non-empty string filename")
    name = value.strip()
    if Path(name).name != name:
        raise CompileError(
            f"{context}: 'oracle' must be a bare filename (no path segments) resolved "
            f"under {ORACLE_DIR}, got {name!r}"
        )
    path = ORACLE_DIR / name
    if not path.is_file():
        raise CompileError(f"{context}: oracle file not found: {path}")
    return path


def lookup_qjson(qjson_spec, label, qjson_by_name):
    name = qjson_spec.get("name") if isinstance(qjson_spec, dict) else None
    if not name:
        raise CompileError(f"entry '{label}': qjson.name is required when qjson is given")
    candidates = qjson_by_name.get(name)
    if not candidates:
        raise CompileError(f"entry '{label}': qjson.name '{name}' not found in q.json")
    index = qjson_spec.get("index", 0)
    if not isinstance(index, int) or isinstance(index, bool):
        raise CompileError(f"entry '{label}': qjson.index must be an int, got {index!r}")
    if index < 0 or index >= len(candidates):
        raise CompileError(
            f"entry '{label}': qjson.index {index} out of range for '{name}' "
            f"({len(candidates)} entr{'y' if len(candidates) == 1 else 'ies'} with that Name; "
            f"q.json has duplicate-named entries for some moves, e.g. fast vs. long-charge "
            f"Dashing Head/Leg variants - disambiguate with an explicit qjson.index)"
        )
    return candidates[index]


def resolve_from_qjson(field, outcome, qentry, label):
    if field in QJSON_FIELD_FOR:
        raw = qentry[QJSON_FIELD_FOR[field]]
    else:  # field == "adv"
        if outcome == "BLOCK":
            raw = qentry["Block_advantage"]
        elif outcome == "HIT":
            raw = qentry["Hit_advantage"]
        else:
            raise CompileError(
                f"entry '{label}': expect.adv = from-qjson is invalid for outcome {outcome} "
                f"(q.json has no whiff advantage field)"
            )
    try:
        return int(raw)
    except (TypeError, ValueError):
        raise CompileError(
            f"entry '{label}': q.json field for '{field}' on '{qentry['Name']}' is non-numeric "
            f"({raw!r}) - cannot use from-qjson; give an explicit expect.{field} instead"
        )


# --- corpus loading/validation ---------------------------------------------

def load_corpus(path):
    with open(path, "r") as f:
        data = yaml.safe_load(f)
    if isinstance(data, list):
        char_id = resolve_character(DEFAULT_CHARACTER, "corpus 'character'")
        oracle_path = resolve_oracle_path(DEFAULT_ORACLE, "corpus 'oracle'")
        return data, DEFAULT_INTER_ENTRY_WAIT, char_id, oracle_path, None, None, DEFAULT_BALANCE, None
    if isinstance(data, dict):
        entries = data.get("entries")
        if not isinstance(entries, list):
            raise CompileError("corpus file is a mapping but has no top-level 'entries' list")
        inter_entry_wait = data.get("inter_entry_wait", DEFAULT_INTER_ENTRY_WAIT)
        if not isinstance(inter_entry_wait, int) or inter_entry_wait <= 0:
            raise CompileError("top-level 'inter_entry_wait' must be a positive int")
        char_id = resolve_character(data.get("character", DEFAULT_CHARACTER), "corpus 'character'")
        oracle_path = resolve_oracle_path(data.get("oracle", DEFAULT_ORACLE), "corpus 'oracle'")
        super_art = resolve_super_art(data.get("super_art"), "corpus 'super_art'")
        sa_gauge = resolve_sa_gauge(data.get("sa_gauge"), "corpus 'sa_gauge'")
        balance = resolve_balance(data.get("balance"), "corpus 'balance'")
        super_full = resolve_super_full(data.get("super_full"), "corpus 'super_full'")
        return entries, inter_entry_wait, char_id, oracle_path, super_art, sa_gauge, balance, super_full
    raise CompileError("corpus file must be a YAML list of entries (optionally a mapping with an 'entries' key)")


def validate_entry_shape(entry, seen_labels):
    if "label" not in entry or not isinstance(entry["label"], str) or not entry["label"]:
        raise CompileError(f"entry missing a non-empty string 'label': {entry!r}")
    label = entry["label"]
    if len(label) > LABEL_MAX_LEN:
        raise CompileError(
            f"entry '{label}': label exceeds {LABEL_MAX_LEN} characters "
            f"(the C-side .fdi parser's hard limit)"
        )
    if not LABEL_RE.match(label):
        raise CompileError(
            f"entry '{label}': label must match {LABEL_RE.pattern} "
            f"(letters, digits, '.', '_', '-' only - no '#', whitespace, or other punctuation)"
        )
    if label in seen_labels:
        raise CompileError(f"duplicate label '{label}'")
    seen_labels.add(label)

    if "input" not in entry or not isinstance(entry["input"], str) or not entry["input"].strip():
        raise CompileError(f"entry '{label}': missing non-empty string 'input'")

    if "outcome" not in entry or entry["outcome"] not in OUTCOMES:
        raise CompileError(f"entry '{label}': 'outcome' must be one of {OUTCOMES}")

    expect = entry.get("expect") or {}
    if not isinstance(expect, dict):
        raise CompileError(f"entry '{label}': 'expect' must be a mapping")
    xfail = expect.get("xfail")
    if xfail is not None and not isinstance(xfail, str):
        raise CompileError(f"entry '{label}': expect.xfail must be a string reason")

    if entry["outcome"] == "NONE":
        given = [f for f in EXPECT_NUMERIC_FIELDS if f in expect]
        if given:
            raise CompileError(
                f"entry '{label}': outcome NONE expects zero FINALs, so "
                f"expect.{given[0]} makes no sense (only expect.xfail is allowed)"
            )
        if "finals" in entry:
            raise CompileError(
                f"entry '{label}': 'finals' is invalid for outcome NONE "
                f"(NONE always expects zero FINALs)"
            )

    finals = entry.get("finals")
    if finals is not None and (not isinstance(finals, int) or isinstance(finals, bool) or finals < 1):
        raise CompileError(f"entry '{label}': 'finals' must be a positive int, got {finals!r}")


def compile_entry(entry, qjson_by_name):
    label = entry["label"]
    expect = entry.get("expect") or {}
    xfail = expect.get("xfail")

    qjson_entry = None
    if "qjson" in entry:
        qjson_entry = lookup_qjson(entry["qjson"], label, qjson_by_name)
    elif any(expect.get(f) == "from-qjson" for f in EXPECT_NUMERIC_FIELDS):
        raise CompileError(f"entry '{label}': expect.* references from-qjson but no qjson.name given")

    resolved = {"outcome": entry["outcome"], "xfail": xfail, "finals": entry.get("finals", 1)}
    for field in EXPECT_NUMERIC_FIELDS:
        if field not in expect:
            resolved[field] = None
            continue
        val = expect[field]
        if field == "kd":
            # kd never goes through resolve_from_qjson's generic "else: #
            # field == adv" branch - it is explicit-only (see the
            # EXPECT_NUMERIC_FIELDS comment above).
            if val == "from-qjson":
                raise CompileError(
                    f"entry '{label}': expect.kd = from-qjson is not supported - "
                    f"kd expectations are explicit corpus authorship, give an "
                    f"explicit expect.kd: 0 or 1"
                )
            if not isinstance(val, int) or isinstance(val, bool) or val not in (0, 1):
                raise CompileError(f"entry '{label}': expect.kd must be 0 or 1, got {val!r}")
            resolved[field] = val
            continue
        if val == "from-qjson":
            resolved[field] = resolve_from_qjson(field, entry["outcome"], qjson_entry, label)
        elif isinstance(val, int) and not isinstance(val, bool):
            resolved[field] = val
        else:
            raise CompileError(
                f"entry '{label}': expect.{field} must be an int or the string 'from-qjson', got {val!r}"
            )

    dummy_mode = resolve_setup_dummy(entry)
    p1_x, p2_x = resolve_setup_dist(entry)
    input_words = parse_input_commands(entry["input"], label)

    script_lines = [
        f"G {dummy_mode}",
        f"P {p1_x} {p2_x}",
        f"L {label}",
    ]
    for word, frames in input_words:
        if frames <= 0:
            continue
        script_lines.append(f"W {word:04x} 0000 {frames}")

    return script_lines, resolved


def compile_corpus(corpus_path, out_dir):
    entries, inter_entry_wait, char_id, oracle_path, super_art, sa_gauge, balance, super_full = load_corpus(
        corpus_path
    )
    if not entries:
        raise CompileError("corpus has no entries")

    qjson_by_name = load_qjson(oracle_path)

    seen_labels = set()
    for entry in entries:
        if not isinstance(entry, dict):
            raise CompileError(f"corpus entry is not a mapping: {entry!r}")
        validate_entry_shape(entry, seen_labels)

    script_lines = [
        f"# Generated by tools/frame-data/compile_corpus.py from {corpus_path}. Do not edit by hand.",
    ]
    expected = {}
    for entry in entries:
        entry_lines, resolved = compile_entry(entry, qjson_by_name)
        script_lines.extend(entry_lines)
        script_lines.append(f"W 0000 0000 {inter_entry_wait}")
        expected[entry["label"]] = resolved
    script_lines.append("Q")

    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "script.fdi").write_text("\n".join(script_lines) + "\n")
    (out_dir / "expected.json").write_text(json.dumps(expected, indent=2) + "\n")
    # run.sh reads this to pass --test-p1-character through to the game
    # invocation (Phase 6 Step 3) - the corpus's `character:` key is the
    # single source of truth, run.sh gets no new CLI surface of its own.
    # p1_super_art / sa_gauge (EX/Supers Step 1 procedure item 4) are added
    # ONLY when the corpus set them - keeps meta.json byte-identical for
    # every corpus that doesn't use the meter mechanism.
    # `balance` is unconditional (task #108) - see resolve_balance above.
    meta = {"p1_character": char_id, "balance": balance}
    if super_art is not None:
        meta["p1_super_art"] = super_art
    if sa_gauge is not None:
        meta["sa_gauge"] = sa_gauge
    if super_full is not None:
        meta["p1_super_full"] = super_full
    (out_dir / "meta.json").write_text(json.dumps(meta, indent=2) + "\n")


def main(argv):
    if len(argv) != 2:
        print("usage: compile_corpus.py <corpus.yaml> <output-dir>", file=sys.stderr)
        return 2
    corpus_path, out_dir = argv
    try:
        compile_corpus(corpus_path, out_dir)
    except CompileError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    except (yaml.YAMLError, OSError) as e:
        print(f"error: could not read corpus '{corpus_path}': {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
