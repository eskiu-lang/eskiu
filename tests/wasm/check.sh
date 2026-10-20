#!/usr/bin/env bash
#
# The run tests on WebAssembly: each tests/NAME.esk with a NAME.expected (and no C
# companion) is compiled for wasm32-unknown-emscripten, linked by emcc and run under
# node; its output and exit code must match the native ones. A test the compiler
# rejects for the target (exceptions, or a module with no WebAssembly port) is
# skipped, and so is one listed in tests/wasm/skip.txt with its reason.
#
# Usage: tests/wasm/check.sh            (needs emcc and node on PATH)
#        ESKIUC=/path/eskiuc tests/wasm/check.sh [NAME...]
set -u
here="$(cd "$(dirname "$0")/.." && pwd)"
root="$(cd "$here/.." && pwd)"
ESKIUC="${ESKIUC:-$root/build/eskiuc}"
unset CC
if ! command -v emcc >/dev/null || ! command -v node >/dev/null; then
    echo "wasm: skipped (emcc or node not found)"
    exit 0
fi
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
skip_list="$(dirname "$0")/skip.txt"
pass=0; fail=0; skipped=0; failed=()
names=("$@")
if [[ ${#names[@]} -eq 0 ]]; then
    for e in "$here"/*.esk; do names+=("$(basename "$e" .esk)"); done
fi
for name in "${names[@]}"; do
    esk="$here/$name.esk"
    [[ -f "$here/$name.expected" && ! -f "$here/$name.c" ]] || continue
    if [[ -f "$skip_list" ]] && grep -q "^$name\b" "$skip_list"; then
        skipped=$((skipped+1)); continue
    fi
    if ! "$ESKIUC" "$esk" --target wasm32-unknown-emscripten -o "$work/$name.js" \
            --link-arg=-sEXIT_RUNTIME=1 --link-arg=-sALLOW_MEMORY_GROWTH=1 \
            --link-arg=-sSTACK_SIZE=8MB >"$work/cerr" 2>&1; then
        if grep -q "not supported on WebAssembly" "$work/cerr"; then
            skipped=$((skipped+1)); continue
        fi
        echo "  FAIL  $name: compile: $(grep -m1 -i error "$work/cerr")"
        fail=$((fail+1)); failed+=("$name"); continue
    fi
    # 30 s at most per test (perl's alarm: macOS has no timeout(1)).
    (cd "$here" && perl -e 'alarm shift; exec @ARGV' 30 node "$work/$name.js" >"$work/out" 2>&1)
    code=$?
    if ! diff -q "$work/out" "$here/$name.expected" >/dev/null; then
        echo "  FAIL  $name: output differs (exit $code)"
        diff "$here/$name.expected" "$work/out" | head -4 | sed 's/^/        /'
        fail=$((fail+1)); failed+=("$name"); continue
    fi
    pass=$((pass+1))
done
echo "wasm: $pass passed, $fail failed, $skipped skipped"
[[ $fail -eq 0 ]]
