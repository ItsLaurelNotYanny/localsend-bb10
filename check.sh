#!/usr/bin/env bash
# Project-gate: configure stack checks before use. Checks must not modify source.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
FAST=0
case "${1:-}" in
    "") [ "$#" -eq 0 ] || { echo "ERROR: unexpected arguments" >&2; exit 2; } ;;
    --fast) [ "$#" -eq 1 ] || { echo "ERROR: unexpected arguments" >&2; exit 2; }; FAST=1 ;;
    *) echo "Usage: ./check.sh [--fast]" >&2; exit 2 ;;
esac
MODE=full
[ "$FAST" -eq 0 ] || MODE=fast
umask 077
LOG=$(mktemp "${TMPDIR:-/tmp}/project-gate.XXXXXX")
trap 'echo "ERROR: gate interrupted; log=$LOG" >&2; exit 2' INT TERM
need() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "ERROR: missing required tool: $1; log=$LOG" >&2
        exit 2
    fi
}
COUNT=0
run() {
    local label=$1 status
    shift
    printf '\n--- %s ---\n' "$label" >>"$LOG"
    if "$@" >>"$LOG" 2>&1; then
        COUNT=$((COUNT + 1))
    else
        status=$?
        printf 'FAIL: %s (exit=%s); log=%s\n' "$label" "$status" "$LOG" >&2
        tail -n 60 "$LOG" | { head -c 8000; cat >/dev/null; } >&2
        printf '\n' >&2
        case "$status" in 126|127) exit 2 ;; *) exit 1 ;; esac
    fi
}
# --- stack checks ---
need docker
CONTAINER_IMAGE="localsend-dev:latest"

# 1. Privacy Boundary Check (ensure no private paths/capture files or personal identifiers in tracked files)
check_privacy_boundaries() {
    local tracked_files
    tracked_files=$(git ls-files)
    
    # Check for forbidden tracked file patterns
    local forbidden_files
    forbidden_files=$(echo "$tracked_files" | grep -E '\.(log|jsonl|pcap)$|AGENTS|review-issues' || true)
    if [ -n "$forbidden_files" ]; then
        echo "ERROR: Private files are tracked in Git:" >&2
        echo "$forbidden_files" >&2
        return 1
    fi

    # Check for forbidden personal strings in tracked files
    local forbidden_strings
    local pattern
    pattern="/home/""ellery|192\.168\.3\.124"
    forbidden_strings=$(git grep -E "$pattern" -- ':!check.sh' || true)
    if [ -n "$forbidden_strings" ]; then
        echo "ERROR: Personal or real device credentials found in tracked files:" >&2
        echo "$forbidden_strings" >&2
        return 1
    fi
    return 0
}

run "privacy-boundary-check" check_privacy_boundaries

if [ "$FAST" -eq 1 ]; then
    run "build-host-tests" docker run --rm -v "$(pwd):/workspace:z" "$CONTAINER_IMAGE" bash -c "cd /workspace/tests && qmake && make -j\$(nproc)"
else
    run "build-host-tests" docker run --rm -v "$(pwd):/workspace:z" "$CONTAINER_IMAGE" bash -c "cd /workspace/tests && qmake && make -j\$(nproc)"
    run "run-host-tests" docker run --rm -v "$(pwd):/workspace:z" "$CONTAINER_IMAGE" bash -c "cd /workspace/tests && ./core-check"
fi

# --- custom checks (preserve during refresh) ---
[ "$COUNT" -gt 0 ] || { echo "ERROR: no checks ran; log=$LOG" >&2; exit 2; }
printf 'OK: project-gate (%s, checks=%s)\n' "$MODE" "$COUNT"
rm -f -- "$LOG"
