#!/usr/bin/env bash
# The self-hosted compiler built through its own EIR C backend (no LLVM IR) still works:
#
#   1. eskiuc-esk --backend=eir builds selfhost/esk_main.esk into cc1;
#   2. cc1 --emit-c of the compiler is byte-identical to eskiuc-esk's (a fixpoint of the C);
#   3. cc1 builds and runs a sample of the run tests through --backend=eir.
#
#   tests/selfhost/eir_bootstrap.sh
#
# ESKIUC_ESK picks the self-hosted compiler (default build/eskiuc-esk); the C compiler is
# $CC, then $CLANG, then cc, as the driver picks it.

set -u
cd "$(dirname "$0")/../.." || exit 2
export ESKIU_ROOT="$(pwd)"
SELF="${ESKIUC_ESK:-build/eskiuc-esk}"
[ -x "$SELF" ] || { echo "eir_bootstrap: cannot find $SELF (set ESKIUC_ESK or build it)"; exit 2; }

work="$(mktemp -d -t eskiu_eirboot.XXXXXX)"
trap 'rm -rf "$work"' EXIT

if ! "$SELF" selfhost/esk_main.esk --backend=eir -O1 -o "$work/cc1" >"$work/build.log" 2>&1; then
    echo "eir_bootstrap: FAILED to build the compiler through EIR"; head -20 "$work/build.log"; exit 1
fi
"$SELF" selfhost/esk_main.esk --emit-c -o "$work/c0.c" || { echo "eir_bootstrap: --emit-c failed"; exit 1; }
"$work/cc1" selfhost/esk_main.esk --emit-c -o "$work/c1.c" || { echo "eir_bootstrap: cc1 --emit-c failed"; exit 1; }
if ! cmp -s "$work/c0.c" "$work/c1.c"; then
    echo "eir_bootstrap: FAILED, cc1 writes other C for the compiler:"; diff "$work/c0.c" "$work/c1.c" | head -10; exit 1
fi
echo "ok    cc1 (built through EIR) writes the same C for the compiler"

fail=0
total=0
for n in arithmetic closures exceptions generic_dot_call async_basic interfaces regex json_doc \
         eir_prototype_closure templates_result threading strings; do
    [ -f "tests/$n.expected" ] || continue
    total=$((total + 1))
    if "$work/cc1" "tests/$n.esk" --backend=eir -o "$work/$n" >"$work/$n.err" 2>&1 \
       && "$work/$n" >"$work/$n.out" 2>&1 && cmp -s "$work/$n.out" "tests/$n.expected"; then
        echo "ok    $n"
    else
        echo "FAIL  $n"; head -5 "$work/$n.err"; fail=1
    fi
done
echo "eir_bootstrap: $total programs built by cc1, $([ $fail -eq 0 ] && echo "all match" || echo "MISMATCH")"
exit "$fail"
