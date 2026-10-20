#!/usr/bin/env bash
# asan_selfhost.sh: build the self-hosted compiler (esk_main.esk) with AddressSanitizer
# and run it over the whole corpus (tests, errors, warnings, the self-host's own sources,
# examples, the stdlib) in --test-codegen mode. Any ASan report (a heap overflow, a use
# after free, a bad read in the compiler itself) fails the gate. Leaks are not checked:
# the compiler frees little on purpose, it exits right after.
set -u
cd "$(dirname "$0")/../.." || exit 2
BIN="${ESKIUC:-build/eskiuc}"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
if ! "$BIN" selfhost/esk_main.esk --asan -O1 -o "$WORK/esk_asan" > "$WORK/b.log" 2>&1; then
    echo "asan selfhost: could not build the ASan compiler"; cat "$WORK/b.log"; exit 2
fi
n=0; bad=0
for f in tests/*.esk tests/errors/*.esk tests/warnings/*.esk selfhost/*.esk examples/*.esk stdlib/*.esk stdlib/c/*.esk; do
    [ -f "$f" ] || continue
    n=$((n + 1))
    ESKIU_ROOT="$PWD" ASAN_OPTIONS=detect_leaks=0 "$WORK/esk_asan" "$f" --test-codegen > /dev/null 2> "$WORK/err"
    if grep -q "AddressSanitizer" "$WORK/err"; then
        bad=$((bad + 1)); echo "FAIL  $f"; grep -m1 "ERROR: AddressSanitizer" "$WORK/err"
    fi
done
echo "----"
if [ "$bad" -eq 0 ]; then echo "asan selfhost: $n files, no reports"; else echo "asan selfhost: $bad of $n files with reports"; exit 1; fi
