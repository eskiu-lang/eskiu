#!/usr/bin/env bash
# The editor modes on a script with a dozen stdlib imports (what an editor waits for on
# each keystroke), against a budget: each mode must finish within its limit, about four
# times what CI's Linux runner takes (typechecker and completion ~130 ms, symbols ~80 ms,
# fmt --check ~10 ms in October 2026), so a slowdown that an editor would feel fails the
# build while runner noise does not. Prints every time either way. The budgets are for
# a Release build of eskiuc (CI's); an unoptimized one takes several times longer.
#
# Usage: tests/editor_timing.sh            (ESKIUC selects the compiler)
set -u
cd "$(dirname "$0")/.." || exit 2
BIN="${ESKIUC:-build/eskiuc}"
f=examples/scripts/organize.esk
fail=0

# Milliseconds since the epoch (GNU date has %N; elsewhere python3).
now_ms() {
    local n; n="$(date +%s%N 2>/dev/null)"
    if [[ "$n" == *N ]]; then python3 -c 'import time; print(int(time.time() * 1000))'
    else echo $((n / 1000000)); fi
}

check() {
    local budget="$1"; shift
    local s e
    s="$(now_ms)"; "$@" > /dev/null 2>&1; e="$(now_ms)"
    local ms=$((e - s))
    if [ "$ms" -le "$budget" ]; then echo "ok    ${*:2}: $ms ms (budget $budget)"
    else echo "SLOW  ${*:2}: $ms ms (budget $budget)"; fail=1; fi
}

"$BIN" "$f" --symbols > /dev/null 2>&1   # warm the file cache
check 600 "$BIN" "$f" --test-typechecker
check 600 "$BIN" "$f" --complete-at 79:20
check 400 "$BIN" "$f" --symbols
check 100 "$BIN" fmt --check "$f"
exit "$fail"
