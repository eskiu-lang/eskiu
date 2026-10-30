#!/usr/bin/env bash
# `fmt` parity gate for the self-hosted compiler driver (selfhost/esk_main.esk).
#
# For every source file in the corpus, format a copy with the C++ `eskiuc fmt` and
# another copy with the self-hosted `esk_main fmt`, then diff. The self-host formatter
# must be BYTE-identical to the C++ one everywhere. Because the C++ formatter is
# idempotent, byte-equality here means the self-host formatter is idempotent too.
#
# Usage: tests/selfhost/fmt_parity.sh [file.esk ...]
#   no args -> stdlib/ + selfhost/ + tests/ + examples/ (the whole in-repo corpus)
# Green (exit 0) = identical formatting for every file.

set -u
cd "$(dirname "$0")/../.." || exit 2
ROOT="$(pwd)"

BIN="${ESKIUC:-}"
if [ -z "$BIN" ]; then
    if [ -x build/eskiuc ]; then BIN=build/eskiuc
    elif command -v eskiuc >/dev/null 2>&1; then BIN=eskiuc
    else echo "fmt_parity: cannot find eskiuc (set ESKIUC or build it)"; exit 2; fi
fi
CLANG="${CLANG:-clang}"   # only used to build the self-host driver
export CLANG
command -v "$CLANG" >/dev/null 2>&1 || { echo "fmt_parity: $CLANG not found (set CLANG)"; exit 2; }

# Build the self-hosted driver with the C++ compiler.
ESKMAIN="$(mktemp -t esk_main.XXXXXX)"
WORK="$(mktemp -d)"
trap 'rm -f "$ESKMAIN"; rm -rf "$WORK"' EXIT
if ! "$BIN" selfhost/esk_main.esk -o "$ESKMAIN" >/dev/null 2>"$WORK/build.log"; then
    echo "fmt_parity: failed to build selfhost/esk_main.esk"; cat "$WORK/build.log"; exit 2
fi

if [ "$#" -gt 0 ]; then
    files=("$@")
else
    files=()
    while IFS= read -r f; do files+=("$f"); done < <(find stdlib selfhost tests examples -name '*.esk' 2>/dev/null | sort)
fi

fail=0
total=0
for f in "${files[@]}"; do
    [ -f "$f" ] || { echo "MISS  $f"; fail=1; continue; }
    total=$((total + 1))
    cp "$f" "$WORK/a.esk"; cp "$f" "$WORK/b.esk"
    "$BIN"     fmt "$WORK/a.esk" >/dev/null 2>&1     # C++ reference
    "$ESKMAIN" fmt "$WORK/b.esk" >/dev/null 2>&1     # self-host
    if ! diff -q "$WORK/a.esk" "$WORK/b.esk" >/dev/null; then
        echo "FAIL  $f"; diff "$WORK/a.esk" "$WORK/b.esk" | head -6; fail=1
    fi
    # The wrap pass (`fmt --width 80`) is byte-identical too.
    cp "$f" "$WORK/a.esk"; cp "$f" "$WORK/b.esk"
    "$BIN"     fmt --width 80 "$WORK/a.esk" >/dev/null 2>&1
    "$ESKMAIN" fmt --width 80 "$WORK/b.esk" >/dev/null 2>&1
    if ! diff -q "$WORK/a.esk" "$WORK/b.esk" >/dev/null; then
        echo "FAIL  $f (--width 80)"; diff "$WORK/a.esk" "$WORK/b.esk" | head -6; fail=1
    fi
done

echo "----"
if [ "$fail" -eq 0 ]; then echo "fmt parity: $total/$total files match"; else echo "fmt parity: MISMATCH"; fi
exit "$fail"
