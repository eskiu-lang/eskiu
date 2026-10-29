#!/usr/bin/env bash
# Byte parity of `--test-eir` between the C++ compiler (eskiuc) and the self-hosted one
# (eskiuc-esk, selfhost/eir.esk) over the run tests (tests/NAME.esk with a NAME.expected).
#
#   tests/selfhost/eir_parity.sh [file.esk ...]
#
# The tests whose EIR text is identical are listed in tests/selfhost/eir_parity_pass.txt:
# one that stops matching fails the gate, one that starts matching is reported (UPDATE=1
# rewrites the list). With files given, each is compared and its first difference shown.

set -u
cd "$(dirname "$0")/../.." || exit 2

BIN="${ESKIUC:-}"
if [ -z "$BIN" ]; then
    if [ -x build/eskiuc ]; then BIN=build/eskiuc
    elif command -v eskiuc >/dev/null 2>&1; then BIN=eskiuc
    else echo "eir_parity: cannot find eskiuc (set ESKIUC or build it)"; exit 2; fi
fi
SELF="${ESKIUC_ESK:-}"
if [ -z "$SELF" ]; then
    if [ -x build/eskiuc-esk ]; then SELF=build/eskiuc-esk
    else echo "eir_parity: cannot find eskiuc-esk (set ESKIUC_ESK or build it)"; exit 2; fi
fi

work="$(mktemp -d -t eskiu_eirp.XXXXXX)"
trap 'rm -rf "$work"' EXIT

if [ $# -gt 0 ]; then
    fail=0
    for f in "$@"; do
        n="$(basename "$f" .esk)"
        "$BIN" "$f" --test-eir >"$work/$n.cpp" 2>/dev/null
        "$SELF" "$f" --test-eir >"$work/$n.esk" 2>/dev/null
        if cmp -s "$work/$n.cpp" "$work/$n.esk" && [ -s "$work/$n.cpp" ]; then
            echo "ok    $n"
        else
            echo "FAIL  $n"
            diff "$work/$n.cpp" "$work/$n.esk" | head -n 20
            fail=1
        fi
    done
    exit "$fail"
fi

list="tests/selfhost/eir_parity_pass.txt"
matched=()
total=0
for f in tests/*.esk; do
    n="$(basename "$f" .esk)"
    [ -f "tests/$n.expected" ] || continue
    total=$((total + 1))
    "$BIN" "$f" --test-eir >"$work/$n.cpp" 2>/dev/null || continue
    "$SELF" "$f" --test-eir >"$work/$n.esk" 2>/dev/null || continue
    cmp -s "$work/$n.cpp" "$work/$n.esk" && matched+=("$n")
done

printf '%s\n' "${matched[@]+"${matched[@]}"}" | sed '/^$/d' | sort > "$work/now.txt"
if [ "${UPDATE:-0}" = 1 ]; then
    cp "$work/now.txt" "$list"
    echo "eir_parity: wrote $list (${#matched[@]}/$total identical)"
    exit 0
fi
[ -f "$list" ] || : > "$list"
lost="$(comm -23 "$list" "$work/now.txt")"
gained="$(comm -13 "$list" "$work/now.txt")"
[ -n "$gained" ] && echo "eir_parity: now identical (UPDATE=1 to record): $(echo $gained)"
echo "eir_parity: ${#matched[@]}/$total identical"
if [ -n "$lost" ]; then
    echo "eir_parity: FAILED, no longer identical: $(echo $lost)"
    exit 1
fi
exit 0
