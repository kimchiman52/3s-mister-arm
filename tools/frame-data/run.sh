#!/usr/bin/env bash
set -euo pipefail

# Single entry point for the frame-data harness (docs/plan-frame-data-harness.md
# section 1.7, Step H5): build -> compile corpus -> run scripted training match
# -> diff trace against expected.json. Exit 0 = green; nonzero = a real diff or
# a harness-plumbing failure (see messages below for which).
#
# Usage: tools/frame-data/run.sh [corpus.yaml]
#   corpus.yaml defaults to tools/frame-data/corpus-q.yaml (Step H6; may not
#   exist yet - pass tools/frame-data/corpus-smoke.yaml explicitly until then).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

CORPUS_PATH="${1:-${REPO_ROOT}/tools/frame-data/corpus-q.yaml}"
BUILD_DIR="${REPO_ROOT}/build/host"
BIN_PATH="${BUILD_DIR}/3S-ARM.app/Contents/MacOS/3S-ARM"

if [ ! -f "$CORPUS_PATH" ]; then
    echo "error: corpus file not found: $CORPUS_PATH" >&2
    exit 1
fi

# macOS has no built-in `timeout`; prefer GNU coreutils' `timeout`/`gtimeout`
# (Homebrew) and fall back to a perl alarm() wrapper that mimics GNU
# timeout's exit code 124 on expiry, so a hung game can never hang the harness.
TIMEOUT_CMD=()
if command -v timeout >/dev/null 2>&1; then
    TIMEOUT_CMD=(timeout)
elif command -v gtimeout >/dev/null 2>&1; then
    TIMEOUT_CMD=(gtimeout)
fi

run_with_timeout() {
    local seconds="$1"
    shift
    if [ "${#TIMEOUT_CMD[@]}" -gt 0 ]; then
        "${TIMEOUT_CMD[@]}" "$seconds" "$@"
    else
        # Must NOT exec() the child in this same perl process: exec() replaces
        # the process image, which resets any caught signal (our ALRM handler)
        # to its default disposition (SIG_DFL) in the new image. If the alarm
        # then fires against the execed child, the kernel's default action for
        # SIGALRM (terminate) applies, so this shell would observe exit 142
        # (128+14) instead of the 124 run.sh's exit-code checks expect - a
        # timeout would be misreported as "game exited with unexpected code
        # 142" rather than the intended "timeout" branch. Fork the child
        # instead, so this perl process's own handler survives to translate
        # the timeout into exit 124 itself.
        perl -e '
            my $seconds = shift @ARGV;
            my $pid = fork();
            if (!defined $pid) { exit 127; }
            if ($pid == 0) {
                exec @ARGV or exit 127;
            }
            local $SIG{ALRM} = sub { kill "TERM", $pid; waitpid($pid, 0); exit 124; };
            alarm($seconds);
            waitpid($pid, 0);
            my $status = $?;
            alarm(0);
            if ($status & 127) {
                exit(128 + ($status & 127));
            }
            exit($status >> 8);
        ' "$seconds" "$@"
    fi
}

# FDH_SKIP_BUILD (opt-in, unset by default): run-suite.sh builds build/host
# once up front and sets this for every fanned-out per-corpus call, so all
# corpora provably run against the same binary instead of each re-running
# this configure+build block. No-op (this block always runs) when unset.
if [ -z "${FDH_SKIP_BUILD:-}" ]; then
    if [ ! -d "$BUILD_DIR" ]; then
        echo "[run.sh] build/host missing, configuring..." >&2
        CC=clang cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug
    fi

    echo "[run.sh] building 3S-ARM (build/host, same-binary protocol)..." >&2
    cmake --build "$BUILD_DIR" --parallel 8
fi

if [ ! -x "$BIN_PATH" ]; then
    echo "error: expected binary not found at $BIN_PATH after build" >&2
    exit 1
fi

# FDH_RUNDIR (opt-in, unset by default): lets run-suite.sh pre-create an
# isolated per-corpus dir and know its path ahead of time. Falls back to a
# fresh mktemp -d, today's behavior, when unset.
RUNDIR="${FDH_RUNDIR:-$(mktemp -d)}"
echo "[run.sh] RUNDIR=$RUNDIR" >&2

echo "[run.sh] compiling corpus $CORPUS_PATH -> $RUNDIR" >&2
python3 "${SCRIPT_DIR}/compile_corpus.py" "$CORPUS_PATH" "$RUNDIR"

# Phase 6 Step 3: the corpus's top-level `character:` key (default q) is
# emitted into meta.json by compile_corpus.py; read it back here and pass
# it through as --test-p1-character. run.sh itself gains no new CLI surface
# - the corpus file stays the single source of truth for P1's character.
P1_CHARACTER="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["p1_character"])' "$RUNDIR/meta.json")"

# EX/Supers program, Step 1 procedure item 4: optional `p1_super_art` /
# `sa_gauge` meta.json keys, present only when the corpus's `super_art:` /
# `sa_gauge:` top-level keys asked for them (compile_corpus.py). Threaded as
# a plain (unquoted, deliberately word-split) string rather than a bash
# array so this stays a byte-for-byte no-op - EXTRA_ARGS is empty and
# vanishes entirely from the command line - for every corpus that omits
# both keys (all 19 existing corpora).
EXTRA_ARGS=""
P1_SUPER_ART="$(python3 -c 'import json,sys; v=json.load(open(sys.argv[1])).get("p1_super_art"); print(v if v is not None else "")' "$RUNDIR/meta.json")"
if [ -n "$P1_SUPER_ART" ]; then
    EXTRA_ARGS="$EXTRA_ARGS --test-p1-super-art $P1_SUPER_ART"
fi
SA_GAUGE="$(python3 -c 'import json,sys; v=json.load(open(sys.argv[1])).get("sa_gauge"); print(v if v is not None else "")' "$RUNDIR/meta.json")"
if [ -n "$SA_GAUGE" ]; then
    EXTRA_ARGS="$EXTRA_ARGS --test-training-sa-gauge $SA_GAUGE"
fi

echo "[run.sh] running harness (training-frame-data, pinned RNG, p1_character=$P1_CHARACTER${EXTRA_ARGS:+, extra=$EXTRA_ARGS})..." >&2
set +e
FRAME_TRACE_PATH="$RUNDIR/trace.log" SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    run_with_timeout 300 "$BIN_PATH" --test-enable --test-scene-preset training-frame-data \
    --test-input-script "$RUNDIR/script.fdi" --test-pin-rng --test-p1-character "$P1_CHARACTER" \
    $EXTRA_ARGS
game_exit=$?
set -e

if [ "$game_exit" -eq 3 ]; then
    echo "error: harness could not start (input_script.c rejected the script or mode) - RUNDIR=$RUNDIR" >&2
    exit 1
elif [ "$game_exit" -eq 124 ]; then
    echo "error: game hung and was killed by the timeout - RUNDIR=$RUNDIR" >&2
    exit 1
elif [ "$game_exit" -ne 0 ]; then
    echo "error: game exited with unexpected code $game_exit - RUNDIR=$RUNDIR" >&2
    exit "$game_exit"
fi

echo "[run.sh] checking frame data..." >&2
set +e
python3 "${SCRIPT_DIR}/check_frame_data.py" "$RUNDIR/trace.log" "$RUNDIR/expected.json"
check_exit=$?
set -e

echo "RUNDIR=$RUNDIR" >&2

# FDH_KEEP_RUNDIR (opt-in, unset by default): run-suite.sh needs
# trace.log/expected.json to survive a successful run for golden generation.
# No-op (still cleans up on success) when unset.
if [ "$check_exit" -eq 0 ] && [ -z "${FDH_KEEP_RUNDIR:-}" ]; then
    rm -rf "$RUNDIR"
else
    echo "[run.sh] leaving RUNDIR for inspection: $RUNDIR" >&2
fi

exit "$check_exit"
