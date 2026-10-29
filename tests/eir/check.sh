#!/usr/bin/env bash
# Behavioral gate for the native EIR backends: compile each run test (tests/NAME.esk with a
# NAME.expected, linked with its C companion NAME.c when there is one) without LLVM, run it
# as tests/run.sh does (from the repository root, stdout and stderr together, exit code 0),
# and compare with the expected output.
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

# Run a test for at most 10 seconds. macOS has no `timeout` unless coreutils is installed
# (the CI runner does not), so fall back to perl, whose alarm survives the exec.
run10() {
    if command -v timeout >/dev/null 2>&1; then
        timeout 10 "$@"
    elif command -v gtimeout >/dev/null 2>&1; then
        gtimeout 10 "$@"
    else
        perl -e 'alarm shift @ARGV; exec @ARGV or exit 127' 10 "$@"
    fi
}

passed=()
for f in tests/*.esk; do
    n="$(basename "$f" .esk)"
    [ -f "tests/$n.expected" ] || continue
    exe="$work/$n.out"
    companion=()
    if [ -f "tests/$n.c" ]; then
        "$CC" -c "tests/$n.c" -o "$work/$n.c.o" >/dev/null 2>&1 || continue
        companion=("$work/$n.c.o")
    fi
    if [ "$mode" = asm ]; then
        link=()
        [ ${#companion[@]} -gt 0 ] && link=(--link-arg "${companion[0]}")
        "$BIN" "$f" ${link[@]+"${link[@]}"} -o "$exe" --backend=eir >/dev/null 2>&1 || continue
    else
        "$BIN" "$f" --emit-c -o "$work/$n.c" >/dev/null 2>&1 || continue
        "$CC" -std=c99 -w -fno-strict-aliasing "$work/$n.c" ${companion[@]+"${companion[@]}"} -o "$exe" -lm -lpthread >/dev/null 2>&1 || continue
    fi
    run10 "$exe" >"$work/$n.got" 2>&1 || continue
    diff -q "tests/$n.expected" "$work/$n.got" >/dev/null && passed+=("$n")
done

printf '%s\n' ${passed[@]+"${passed[@]}"} | sed '/^$/d' | sort > "$work/now.txt"
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
