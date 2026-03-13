#!/bin/sh
set -eu

ROOT_DIR="${ROOT_DIR:-/Users/sb/Developer/3sx-mister}"
PROMPT_FILE="${PROMPT_FILE:-$ROOT_DIR/artifacts/mister-port/overnight-perf-prompt.txt}"
LIVING_DOC="${LIVING_DOC:-$ROOT_DIR/artifacts/mister-port/living-findings.md}"
LOG_DIR="${LOG_DIR:-$ROOT_DIR/artifacts/mister-port/logs}"
LOG_FILE="${LOG_FILE:-$LOG_DIR/ralph-loop.log}"
CODEX_BIN="${CODEX_BIN:-}"

BASE_SLEEP_SECONDS="${BASE_SLEEP_SECONDS:-300}"
MAX_BACKOFF_SECONDS="${MAX_BACKOFF_SECONDS:-3600}"
MAX_LOG_BYTES="${MAX_LOG_BYTES:-10485760}"
KEEP_ROTATED_LOGS="${KEEP_ROTATED_LOGS:-12}"

mkdir -p "$LOG_DIR"

if [ -z "$CODEX_BIN" ]; then
    CODEX_BIN="$(command -v codex 2>/dev/null || true)"
fi

if [ -z "$CODEX_BIN" ] && [ -x /opt/homebrew/bin/codex ]; then
    CODEX_BIN="/opt/homebrew/bin/codex"
fi

if [ -z "$CODEX_BIN" ] || [ ! -x "$CODEX_BIN" ]; then
    echo "codex binary not found; set CODEX_BIN explicitly" >&2
    exit 1
fi

git_is_clean() {
    test -z "$(git -C "$ROOT_DIR" status --short)"
}

log_dirty_state() {
    {
        echo "[note] dirty git worktree detected; next Ralph cycle must reconcile it before proceeding"
        git -C "$ROOT_DIR" status --short
    } >>"$LOG_FILE"
}

rotate_logs() {
    if [ ! -f "$LOG_FILE" ]; then
        return 0
    fi

    size="$(wc -c <"$LOG_FILE" | tr -d ' ')"
    if [ "$size" -lt "$MAX_LOG_BYTES" ]; then
        return 0
    fi

    ts="$(date +%Y%m%d-%H%M%S)"
    rotated="$LOG_DIR/ralph-loop-$ts.log"
    mv "$LOG_FILE" "$rotated"
    : >"$LOG_FILE"

    n=0
    for f in $(ls -1t "$LOG_DIR"/ralph-loop-*.log 2>/dev/null); do
        n=$((n + 1))
        if [ "$n" -gt "$KEEP_ROTATED_LOGS" ]; then
            rm -f "$f"
        fi
    done
}

if [ ! -f "$PROMPT_FILE" ]; then
    echo "Prompt file not found: $PROMPT_FILE" >&2
    exit 1
fi

if [ ! -f "$LIVING_DOC" ]; then
    mkdir -p "$(dirname "$LIVING_DOC")"
    cat >"$LIVING_DOC" <<'EOF'
# MiSTer Performance Living Findings

Purpose:
- Preserve high-value optimization context across fresh loop sessions.

## Cycle Log
EOF
fi

BACKOFF="$BASE_SLEEP_SECONDS"

while true; do
    rotate_logs
    OUT="$(mktemp)"
    {
        echo "===== $(date) ====="
        echo "pwd=$ROOT_DIR"
        echo "backoff=${BACKOFF}s"
    } >>"$LOG_FILE"

    if ! git_is_clean; then
        log_dirty_state
    fi

    if "$CODEX_BIN" exec --dangerously-bypass-approvals-and-sandbox -C "$ROOT_DIR" "$(cat "$PROMPT_FILE")" >"$OUT" 2>&1; then
        cat "$OUT" >>"$LOG_FILE"
        if ! git_is_clean; then
            log_dirty_state
        fi
        BACKOFF="$BASE_SLEEP_SECONDS"
        sleep "$BASE_SLEEP_SECONDS"
    else
        cat "$OUT" >>"$LOG_FILE"
        if grep -Eiq "quota|rate limit|429|insufficient_quota" "$OUT"; then
            echo "[backoff] quota/rate limit hit; sleeping ${BACKOFF}s" >>"$LOG_FILE"
            sleep "$BACKOFF"
            BACKOFF=$((BACKOFF * 2))
            if [ "$BACKOFF" -gt "$MAX_BACKOFF_SECONDS" ]; then
                BACKOFF="$MAX_BACKOFF_SECONDS"
            fi
        else
            BACKOFF="$BASE_SLEEP_SECONDS"
            sleep "$BASE_SLEEP_SECONDS"
        fi
    fi

    rm -f "$OUT"
    rotate_logs
done
