#!/usr/bin/env python3
"""Check that the two compiled-in `replay-proxy-host` defaults still agree.

WHY THIS IS A SOURCE CHECK AND NOT A UNIT TEST. The device-side weekly-best
replay refresh needs a proxy host, and the host is written down TWICE in two
trees that no translation unit and no build ever sees together:

  1. vendor/Main_MiSTer/replay_proxy.c -> RP_DEFAULT_PROXY_HOST. This is the
     copy that decides anything. RpConfigLoadFrom() substitutes it for an
     absent or empty `replay-proxy-host`, so it is what reaches every device
     that ALREADY has a config file -- which is every device that has ever run
     the game.
  2. src/port/config/config.c -> DEFAULT_REPLAY_PROXY_HOST, used by the
     CFG_KEY_REPLAY_PROXY_HOST row of default_entries[]. This one only seeds a
     FRESHLY WRITTEN config file, because Config_Init() calls write_defaults()
     solely when fopen() of the file fails.

Nothing links the two: the wrapper is a C++/C HPS binary built by
tools/mister-wrapper/build-hps.sh, the game is a separate ARM binary, and the
only channel between them is an INI file parsed by string. So drift is silent
in both directions, and neither direction is a crash:

  * wrapper moved, game stale -> a fresh install writes a config naming the old
    host, which then WINS over the wrapper default forever (a non-empty value
    is never overridden), so new installs quietly point at a dead address.
  * game moved, wrapper stale -> the installed base keeps using the old host
    while the documented default says otherwise.

The one symptom either way is that no `replay_sync:` lines appear in
logs/last-run.log, which is exactly the failure this default was added to fix
and exactly the failure nobody noticed for the life of the feature.

Same shape, and the same remedy, as check_constant_time_compare.py and
check_versus_score_reset_sites.py: the property is a relationship between two
source literals, so it is checked at the source.

What is asserted:

  A. vendor/Main_MiSTer/replay_proxy.c defines RP_DEFAULT_PROXY_HOST exactly
     once, to a non-empty string literal.
  B. src/port/config/config.c defines DEFAULT_REPLAY_PROXY_HOST exactly once,
     to a non-empty string literal.
  C. The two literals are byte-identical.
  D. The CFG_KEY_REPLAY_PROXY_HOST row of default_entries[] uses the MACRO, not
     an inline literal -- otherwise the macro could stay correct while the row
     that is actually compiled drifts, and A-C would pass vacuously.
  E. Neither literal is the disable sentinel RP_PROXY_HOST_OFF (`off`), which
     would ship the feature switched off while looking configured, and
     RP_PROXY_HOST_OFF is still defined for RpConfigLoadFrom() to compare
     against.
  F. RpConfigLoadFrom() still consumes both macros -- a default nothing applies
     is the original defect, restored.

Exit codes: 0 = the two agree, 1 = they do not, 2 = a file or a definition
could not be located (itself a failure: a check that silently finds nothing is
worse than no check).
"""

import argparse
import pathlib
import re
import sys

TAG = "[replay-proxy-host-default]"


def strip_comments(text):
    """Drop /* */ blocks and // line comments so a prose mention of a macro is
    not a definition. Trailing `//` needs preceding whitespace so a `://`
    inside a string literal survives."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"(?m)^\s*//[^\n]*", "", text)
    text = re.sub(r"(?m)\s//[^\n]*", "", text)
    return text


def find_string_define(text, name):
    """Return (list of string-literal values) for every `#define <name> "..."`.

    A list, not a single value: two definitions of the same macro is itself the
    failure (a platform #if that only sometimes applies), and the caller
    reports the count.
    """
    pat = re.compile(r'(?m)^\s*#\s*define\s+' + re.escape(name) + r'\s+"([^"]*)"\s*$')
    return pat.findall(text)


def extract_function(text, signature_re):
    """Body of the function whose DEFINITION matches signature_re, by brace
    matching from its opening brace; None if not found. Demands a `{` after the
    parameter list with no `;` between, so a prototype does not match and
    brace-match its way into the next function."""
    m = re.search(signature_re + r"[^;{]*\)\s*\{", text, re.M)
    if m is None:
        return None
    start = text.index("{", m.start())
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start + 1:i]
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wrapper", default="vendor/Main_MiSTer/replay_proxy.c")
    ap.add_argument("--game-config", default="src/port/config/config.c")
    args = ap.parse_args()

    wrapper_path = pathlib.Path(args.wrapper)
    game_path = pathlib.Path(args.game_config)
    for p in (wrapper_path, game_path):
        if not p.is_file():
            print(f"{TAG} ERROR: {p} does not exist", file=sys.stderr)
            return 2

    wrapper = strip_comments(wrapper_path.read_text())
    game = strip_comments(game_path.read_text())
    failures = []
    errors = []

    # --- A / B. One definition each, non-empty -----------------------------
    wrapper_vals = find_string_define(wrapper, "RP_DEFAULT_PROXY_HOST")
    game_vals = find_string_define(game, "DEFAULT_REPLAY_PROXY_HOST")

    if len(wrapper_vals) != 1:
        errors.append(
            f"{wrapper_path} has {len(wrapper_vals)} `#define "
            f"RP_DEFAULT_PROXY_HOST \"...\"` definition(s), expected exactly 1 "
            f"-- the wrapper-side default is what reaches every installed "
            f"device, so this check cannot be allowed to guess which one")
    elif wrapper_vals[0] == "":
        failures.append(
            f"{wrapper_path} -> RP_DEFAULT_PROXY_HOST is the empty string, "
            f"which is what RpConfigLoadFrom() treats as DISABLED -- the "
            f"weekly-best refresh would be off on every device, which is the "
            f"defect this default exists to fix")

    if len(game_vals) != 1:
        errors.append(
            f"{game_path} has {len(game_vals)} `#define "
            f"DEFAULT_REPLAY_PROXY_HOST \"...\"` definition(s), expected "
            f"exactly 1")
    elif game_vals[0] == "":
        failures.append(
            f"{game_path} -> DEFAULT_REPLAY_PROXY_HOST is the empty string, so "
            f"a freshly written config file would name no host; the wrapper "
            f"default would still apply, but the file and the wrapper would "
            f"disagree about what the default IS")

    # --- C. Byte-identical -------------------------------------------------
    if len(wrapper_vals) == 1 and len(game_vals) == 1 and wrapper_vals[0] != game_vals[0]:
        failures.append(
            f"the two `replay-proxy-host` defaults have DRIFTED: "
            f"{wrapper_path} -> RP_DEFAULT_PROXY_HOST = \"{wrapper_vals[0]}\" "
            f"but {game_path} -> DEFAULT_REPLAY_PROXY_HOST = "
            f"\"{game_vals[0]}\". A fresh install writes the game's value into "
            f"its config file, and a non-empty value there WINS over the "
            f"wrapper's -- so new installs and existing ones would talk to "
            f"different addresses, and at most one of them can be live")

    # --- D. The table row uses the macro, not an inline literal -------------
    row = re.search(
        r"\.key\s*=\s*CFG_KEY_REPLAY_PROXY_HOST\s*,"
        r"[^}]*?\.value\.s\s*=\s*([A-Za-z_][A-Za-z0-9_]*|\"[^\"]*\")",
        game, re.S)
    if row is None:
        errors.append(
            f"could not find the CFG_KEY_REPLAY_PROXY_HOST row of "
            f"default_entries[] in {game_path} -- without it this check says "
            f"nothing about what is actually compiled")
    elif row.group(1) != "DEFAULT_REPLAY_PROXY_HOST":
        failures.append(
            f"the CFG_KEY_REPLAY_PROXY_HOST row of default_entries[] sets "
            f".value.s = {row.group(1)}, not DEFAULT_REPLAY_PROXY_HOST -- the "
            f"macro could then stay in step with the wrapper while the row that "
            f"is actually compiled drifts away from both, and the equality "
            f"check above would pass vacuously")

    # --- E. Not the disable sentinel ---------------------------------------
    off_vals = find_string_define(wrapper, "RP_PROXY_HOST_OFF")
    if len(off_vals) != 1:
        errors.append(
            f"{wrapper_path} has {len(off_vals)} `#define RP_PROXY_HOST_OFF "
            f"\"...\"` definition(s), expected exactly 1 -- the operator's "
            f"documented off switch (docs/config.md) has no definition to "
            f"compare against")
    else:
        off = off_vals[0]
        for label, path, vals in (
                ("RP_DEFAULT_PROXY_HOST", wrapper_path, wrapper_vals),
                ("DEFAULT_REPLAY_PROXY_HOST", game_path, game_vals)):
            if len(vals) == 1 and vals[0].lower() == off.lower():
                failures.append(
                    f"{path} -> {label} is \"{vals[0]}\", the disable sentinel "
                    f"RP_PROXY_HOST_OFF -- that ships the refresh switched OFF "
                    f"while every doc and every config file says it is "
                    f"configured")

    # --- F. RpConfigLoadFrom still applies both -----------------------------
    loader = extract_function(wrapper, r"^bool\s+RpConfigLoadFrom\s*\(")
    if loader is None:
        errors.append(
            f"could not locate RpConfigLoadFrom in {wrapper_path} -- it is the "
            f"only place the wrapper default is applied, so this check no "
            f"longer knows that it is applied at all")
    else:
        if "RP_DEFAULT_PROXY_HOST" not in loader:
            failures.append(
                "RpConfigLoadFrom() no longer references RP_DEFAULT_PROXY_HOST "
                "-- an empty `replay-proxy-host` is back to meaning disabled, "
                "and the installed base gets no refresh again")
        if "RP_PROXY_HOST_OFF" not in loader:
            failures.append(
                "RpConfigLoadFrom() no longer references RP_PROXY_HOST_OFF -- "
                "the operator has no way to turn the refresh off, and "
                "docs/config.md says they do")

    if errors:
        print(f"{TAG} ERROR: this check could not find what it guards; it now "
              f"guards nothing.", file=sys.stderr)
        for e in errors:
            print(f"{TAG}   - {e}", file=sys.stderr)
        for f in failures:
            print(f"{TAG}   - (also) {f}", file=sys.stderr)
        return 2
    if failures:
        print(f"{TAG} FAIL: the two compiled-in `replay-proxy-host` defaults no "
              f"longer hold.", file=sys.stderr)
        for f in failures:
            print(f"{TAG}   - {f}", file=sys.stderr)
        print(f"{TAG} No build can catch this: the wrapper and the game are "
              f"separate binaries whose only shared channel is an INI file "
              f"parsed by string. The symptom is the absence of `replay_sync:` "
              f"lines in logs/last-run.log.", file=sys.stderr)
        return 1

    print(f"{TAG} PASS: replay-proxy-host default \"{wrapper_vals[0]}\" agrees "
          f"between {wrapper_path} (RP_DEFAULT_PROXY_HOST, applied by "
          f"RpConfigLoadFrom) and {game_path} "
          f"(DEFAULT_REPLAY_PROXY_HOST, used by the "
          f"CFG_KEY_REPLAY_PROXY_HOST row); disable sentinel is "
          f"\"{off_vals[0]}\" and neither default is it.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
