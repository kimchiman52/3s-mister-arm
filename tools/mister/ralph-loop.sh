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
STALL_TIMEOUT_SECONDS="${STALL_TIMEOUT_SECONDS:-900}"
STALL_POLL_SECONDS="${STALL_POLL_SECONDS:-30}"
STALL_KILL_GRACE_SECONDS="${STALL_KILL_GRACE_SECONDS:-10}"
STALL_RESTART_SLEEP_SECONDS="${STALL_RESTART_SLEEP_SECONDS:-30}"

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

file_mtime_seconds() {
    path="$1"
    if [ ! -f "$path" ]; then
        echo 0
        return 0
    fi

    if stat -f %m "$path" >/dev/null 2>&1; then
        stat -f %m "$path"
        return 0
    fi

    stat -c %Y "$path" 2>/dev/null || echo 0
}

file_activity_stamp() {
    path="$1"
    if [ ! -f "$path" ]; then
        echo "missing:0"
        return 0
    fi

    size="$(wc -c <"$path" | tr -d ' ')"
    mtime="$(file_mtime_seconds "$path")"
    printf '%s:%s\n' "$size" "$mtime"
}

find_codex_session_file() {
    pid="$1"
    if ! command -v lsof >/dev/null 2>&1; then
        return 1
    fi

    lsof -Fn -p "$pid" 2>/dev/null |
        sed -n 's/^n//p' |
        grep '/\.codex/sessions/.*\.jsonl$' |
        head -n 1
}

monitor_codex_exec() {
    child_pid="$1"
    out_file="$2"

    WATCHDOG_SESSION_FILE=""
    WATCHDOG_IDLE_SECONDS=0

    last_output_stamp="$(file_activity_stamp "$out_file")"
    last_session_stamp="missing:0"
    last_activity_ts="$(date +%s)"

    while kill -0 "$child_pid" 2>/dev/null; do
        if [ -z "$WATCHDOG_SESSION_FILE" ]; then
            WATCHDOG_SESSION_FILE="$(find_codex_session_file "$child_pid" || true)"
            if [ -n "$WATCHDOG_SESSION_FILE" ]; then
                printf '[watchdog] session file: %s\n' "$WATCHDOG_SESSION_FILE" >>"$LOG_FILE"
                last_session_stamp="$(file_activity_stamp "$WATCHDOG_SESSION_FILE")"
                last_activity_ts="$(date +%s)"
            fi
        fi

        current_output_stamp="$(file_activity_stamp "$out_file")"
        current_session_stamp="$last_session_stamp"
        if [ -n "$WATCHDOG_SESSION_FILE" ]; then
            current_session_stamp="$(file_activity_stamp "$WATCHDOG_SESSION_FILE")"
        fi

        if [ "$current_output_stamp" != "$last_output_stamp" ] || [ "$current_session_stamp" != "$last_session_stamp" ]; then
            last_output_stamp="$current_output_stamp"
            last_session_stamp="$current_session_stamp"
            last_activity_ts="$(date +%s)"
        fi

        now_ts="$(date +%s)"
        WATCHDOG_IDLE_SECONDS=$((now_ts - last_activity_ts))
        if [ "$WATCHDOG_IDLE_SECONDS" -ge "$STALL_TIMEOUT_SECONDS" ]; then
            printf '[watchdog] no transcript or stdout growth for %ss; terminating pid=%s\n' "$WATCHDOG_IDLE_SECONDS" "$child_pid" >>"$LOG_FILE"
            kill "$child_pid" 2>/dev/null || true

            deadline_ts=$((now_ts + STALL_KILL_GRACE_SECONDS))
            while kill -0 "$child_pid" 2>/dev/null; do
                if [ "$(date +%s)" -ge "$deadline_ts" ]; then
                    printf '[watchdog] pid=%s ignored TERM; sending KILL\n' "$child_pid" >>"$LOG_FILE"
                    kill -9 "$child_pid" 2>/dev/null || true
                    break
                fi
                sleep 1
            done

            wait "$child_pid" 2>/dev/null || true
            return 124
        fi

        sleep "$STALL_POLL_SECONDS"
    done

    wait "$child_pid"
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

    "$CODEX_BIN" exec --dangerously-bypass-approvals-and-sandbox -C "$ROOT_DIR" "$(cat "$PROMPT_FILE")" >"$OUT" 2>&1 &
    CODEX_PID=$!
    printf '[watchdog] started codex pid=%s\n' "$CODEX_PID" >>"$LOG_FILE"

    if monitor_codex_exec "$CODEX_PID" "$OUT"; then
        cat "$OUT" >>"$LOG_FILE"
        if ! git_is_clean; then
            log_dirty_state
        fi
        BACKOFF="$BASE_SLEEP_SECONDS"
        sleep "$BASE_SLEEP_SECONDS"
    else
        STATUS="$?"
        cat "$OUT" >>"$LOG_FILE"
        if [ "$STATUS" -eq 124 ]; then
            printf '[watchdog] restarting after stall; session=%s idle=%ss sleep=%ss\n' "${WATCHDOG_SESSION_FILE:-unknown}" "${WATCHDOG_IDLE_SECONDS:-0}" "$STALL_RESTART_SLEEP_SECONDS" >>"$LOG_FILE"
            BACKOFF="$BASE_SLEEP_SECONDS"
            sleep "$STALL_RESTART_SLEEP_SECONDS"
        elif grep -Eiq "quota|rate limit|429|insufficient_quota" "$OUT"; then
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
