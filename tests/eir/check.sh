#!/usr/bin/env bash
# Behavioral gate for the native EIR backends: compile each run test (tests/NAME.esk with a
# NAME.expected and no C companion) without LLVM, run it, and compare its stdout with the
# expected output.
#
#   tests/eir/check.sh [asm|c]
#
# asm: `--backend=eir` (ARM64 assembly; needs an arm64 host). c: `--emit-c`, compiled with
# $CC (default cc). Default: asm on arm64, c elsewhere.
#
# The tests known to pass are listed in tests/eir/pass_<mode>.txt: one that stops passing
# fails the gate, one that starts passing is reported (UPDATE=1 rewrites the list).

set -u
cd "$(dirname "$0")/../.." || exit 2

BIN="${ESKIUC:-build/eskiuc}"
[ -x "$BIN" ] || { echo "eir/check: cannot find eskiuc (set ESKIUC or build it)"; exit 2; }
CC="${CC:-cc}"

mode="${1:-}"
if [ -z "$mode" ]; then
    case "$(uname -m)" in arm64|aarch64) mode=asm ;; *) mode=c ;; esac
fi
case "$mode" in asm|c) ;; *) echo "usage: tests/eir/check.sh [asm|c]"; exit 2 ;; esac

list="tests/eir/pass_$mode.txt"
work="$(mktemp -d -t eskiu_eir.XXXXXX)"
trap 'rm -rf "$work"' EXIT

passed=()
for f in tests/*.esk; do
    n="$(basename "$f" .esk)"
    [ -f "tests/$n.expected" ] || continue
    [ -f "tests/$n.c" ] && continue
    exe="$work/$n.out"
    if [ "$mode" = asm ]; then
        "$BIN" "$f" -o "$exe" --backend=eir >/dev/null 2>&1 || continue
    else
        "$BIN" "$f" --emit-c -o "$work/$n.c" >/dev/null 2>&1 || continue
        "$CC" -std=c99 -w -fno-strict-aliasing "$work/$n.c" -o "$exe" -lm -lpthread >/dev/null 2>&1 || continue
    fi
    out="$(cd tests && timeout 10 "$exe" 2>/dev/null)"
    [ "$out" = "$(cat "tests/$n.expected")" ] && passed+=("$n")
done

printf '%s\n' "${passed[@]}" | sort > "$work/now.txt"
if [ "${UPDATE:-0}" = 1 ]; then
    cp "$work/now.txt" "$list"
    echo "eir/check ($mode): wrote $list (${#passed[@]} passing)"
    exit 0
fi

touch "$list"
lost="$(comm -23 <(sort "$list") "$work/now.txt")"
gained="$(comm -13 <(sort "$list") "$work/now.txt")"
[ -n "$gained" ] && echo "newly passing (UPDATE=1 to record):" && sed 's/^/  /' <<< "$gained"
if [ -n "$lost" ]; then
    echo "FAIL: no longer passing under the $mode backend:"
    sed 's/^/  /' <<< "$lost"
    exit 1
fi
echo "eir/check ($mode): ${#passed[@]} passing, none lost"
