#!/usr/bin/env bash
# Whole-corpus behavioral equivalence of the two compilers.
#
# Runs the ENTIRE positive test corpus (tests/*.esk) through the Eskiu-built compiler
# end to end: its driver (full pipeline, incl. sema) links via clang with no -l flags, so
# the libraries a program implies are the self-host's own; then run, and require the
# same observable result the C++ `eskiuc` produces. A run test (has a `.expected`) must
# exit 0 and print exactly `.expected`; a smoke test (no `.expected`, non-deterministic
# output) must exit 0. The negative corpus (tests/errors/) is covered by tc_parity.sh
# (verdict + diagnostic). Together they are the acceptance gate for making the Eskiu-built
# compiler primary: no behavioral divergence anywhere the C++ compiler is exercised.
#
# Usage: tests/selfhost/corpus_parity.sh
# Green (exit 0) = every positive program behaves identically under the Eskiu compiler.

set -u
cd "$(dirname "$0")/../.." || exit 2
ROOT="$(pwd)"

BIN="${ESKIUC:-}"
if [ -z "$BIN" ]; then
    if [ -x build/eskiuc ]; then BIN=build/eskiuc
    elif command -v eskiuc >/dev/null 2>&1; then BIN=eskiuc
    else echo "corpus_parity: cannot find eskiuc (set ESKIUC or build it)"; exit 2; fi
fi
CLANG="${CLANG:-clang}"   # clang assembles the emitted .ll; CI installs it as clang-22
command -v "$CLANG" >/dev/null 2>&1 || { echo "corpus_parity: $CLANG not found (set CLANG)"; exit 2; }
export CLANG

ESKMAIN="$(mktemp -t esk_main.XXXXXX)"
WORK="$(mktemp -d)"
trap 'rm -f "$ESKMAIN"; rm -rf "$WORK"' EXIT
if ! "$BIN" selfhost/esk_main.esk -o "$ESKMAIN" >/dev/null 2>"$WORK/build.log"; then
    echo "corpus_parity: failed to build selfhost/esk_main.esk"; cat "$WORK/build.log"; exit 2
fi

fail=0; run_ok=0; smoke_ok=0; total=0
source tests/resolver.sh
for esk in tests/*.esk; do
    [ -f "$esk" ] || continue
    name="$(basename "$esk" .esk)"
    resolver_skip "$name" && continue
    total=$((total + 1))
    companion=()                                  # tests/NAME.c: C side of a C-ABI test
    if [ -f "tests/$name.c" ]; then
        "$CLANG" -c "tests/$name.c" -o "$WORK/$name.c.o" 2>"$WORK/$name.clang" || { echo "FAIL  $name  (C companion failed to compile)"; fail=1; continue; }
        companion=(--link-arg "$WORK/$name.c.o")
    fi
    if ! ESKIU_ROOT="$ROOT" "$ESKMAIN" "$esk" ${companion[@]+"${companion[@]}"} -o "$WORK/$name.bin" 2>"$WORK/$name.emit"; then
        echo "FAIL  $name  (self-host compile/link errored)"; sed 's/^/      /' "$WORK/$name.emit" | grep -vi 'overriding the module' | grep -v 'built for newer' | head -3; fail=1; continue
    fi
    ESKIU_ROOT="$ROOT" "$WORK/$name.bin" > "$WORK/$name.out" 2>&1; code=$?
    exp="tests/$name.expected"
    if [ -f "$exp" ]; then
        if [ "$code" -ne 0 ]; then
            echo "FAIL  $name  (exited $code, expected 0)"; fail=1
        elif diff -u "$exp" "$WORK/$name.out" > "$WORK/$name.diff" 2>&1; then
            run_ok=$((run_ok + 1))
        else
            echo "FAIL  $name  (output differs from .expected)"; sed 's/^/      /' "$WORK/$name.diff" | head -8; fail=1
        fi
    else
        if [ "$code" -eq 0 ]; then smoke_ok=$((smoke_ok + 1)); else echo "FAIL  $name  (smoke test exited $code)"; fail=1; fi
    fi
done

echo "----"
if [ "$fail" -eq 0 ]; then
    echo "corpus parity: $total/$total programs match (run: $run_ok, smoke: $smoke_ok)"
else
    echo "corpus parity: MISMATCH"
fi
exit "$fail"
