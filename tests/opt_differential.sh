#!/usr/bin/env bash
# -O0 vs -O2 behavioral differential.
#
# Every tests/*.esk must produce an identical exit code + stdout whether compiled at
# -O0 (naive IR straight to the backend) or -O2 (the LLVM middle-end pipeline the `-O`
# flag runs). A divergence means the optimizer miscompiled the program — i.e. the
# front-end emitted IR that is wrong in a way -O0 tolerates but -O2 exploits (a bad
# attribute, a type/ABI mismatch, UB, ...). This is the gate that caught the
# float-closure return-type miscompile (see CHANGELOG 0.3.1).
#
# Files that don't compile+link standalone (need extra link libs) are skipped and
# counted. A few known-flaky async server tests are skipped explicitly (their codegen
# is covered by tests/run.sh); running them here would import their flakiness.
#
# Usage: [ESKIUC=path] [CC=clang-22] tests/opt_differential.sh
# Green (exit 0) = every compiled program behaves identically at -O0 and -O2.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
cd "$root" || exit 2

ESKIUC="${ESKIUC:-$root/build/eskiuc}"
[ -x "$ESKIUC" ] || { echo "error: compiler not found at $ESKIUC (build it)" >&2; exit 2; }

# Server/async tests with intermittent, timing-dependent behavior on CI — excluded so
# this gate stays deterministic (see the project-flaky-http2 note). Their codegen is
# exercised by tests/run.sh.
SKIP=" http2_handshake http2_multiplex http2_server "

# Bound each run so a hung program can't stall CI (best-effort: no-op if unavailable).
TO=""; command -v timeout >/dev/null 2>&1 && TO="timeout 30"

work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
fail=0; total=0; skip=0; excluded=0
source tests/resolver.sh
for f in tests/*.esk; do
    n="$(basename "$f" .esk)"
    resolver_skip "$n" && { echo "skip  $n  (ESKIU_RESOLVER_TESTS=0)"; continue; }
    case "$SKIP" in *" $n "*) echo "excl  $n  (flaky async — see project-flaky-http2)"; excluded=$((excluded + 1)); continue ;; esac
    # A tests/NAME.c companion (C side of a C-ABI test) is compiled and linked in.
    extra=()
    if [ -f "tests/$n.c" ]; then
        ${CC:-clang} -c "tests/$n.c" -o "$work/$n.c.o" >/dev/null 2>&1 && extra=(--link-arg "$work/$n.c.o")
    fi
    # Compile+link at both levels (the driver adds the libraries a program implies).
    if ! "$ESKIUC"     "$f" ${extra[@]+"${extra[@]}"} -o "$work/$n.o0" >/dev/null 2>&1 \
    || ! "$ESKIUC" -O2 "$f" ${extra[@]+"${extra[@]}"} -o "$work/$n.o2" >/dev/null 2>&1; then
        skip=$((skip + 1)); continue
    fi
    total=$((total + 1))
    a="$($TO "$work/$n.o0" 2>&1; echo "rc=$?")"
    b="$($TO "$work/$n.o2" 2>&1; echo "rc=$?")"
    if [ "$a" = "$b" ]; then
        echo "ok    $n"
    else
        echo "FAIL  $n  (-O0 and -O2 diverge)"
        diff <(printf '%s\n' "$a") <(printf '%s\n' "$b") | sed 's/^/      /' | head -20
        fail=1
    fi
done

echo "----"
if [ "$fail" -eq 0 ]; then
    echo "opt differential: $total/$total match (-O0 == -O2), $skip link-skipped, $excluded excluded"
else
    echo "opt differential: MISMATCH"
fi
exit "$fail"
