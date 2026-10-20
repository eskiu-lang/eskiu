#!/usr/bin/env bash
#
# The run tests on a phone target: each tests/NAME.esk with a NAME.expected (and no C
# companion) is compiled and linked for the target, run on a simulator or emulator,
# and its output compared with the native one. A test listed in tests/mobile/skip.txt
# is skipped (with its reason there).
#
#   PLATFORM=ios     an iOS simulator (xcrun simctl; DEVICE names a booted one, else
#                    the first booted device is used); target arm64-apple-ios15.0-simulator
#   PLATFORM=android a running emulator or device seen by adb; TARGET picks the ABI
#                    (default aarch64-linux-android24)
#
# Usage: PLATFORM=ios tests/mobile/check.sh [NAME...]
set -u
here="$(cd "$(dirname "$0")/.." && pwd)"
root="$(cd "$here/.." && pwd)"
ESKIUC="${ESKIUC:-$root/build/eskiuc}"
PLATFORM="${PLATFORM:-ios}"
unset CC
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
case "$PLATFORM" in
    ios)
        TARGET="${TARGET:-arm64-apple-ios15.0-simulator}"
        DEVICE="${DEVICE:-$(xcrun simctl list devices booted | grep -m1 -oE '[0-9A-F-]{36}')}"
        if [[ -z "$DEVICE" ]]; then echo "mobile: skipped (no booted iOS simulator)"; exit 0; fi
        run() { (cd "$here" && xcrun simctl spawn "$DEVICE" "$1"); } ;;
    android)
        TARGET="${TARGET:-aarch64-linux-android24}"
        ADB="${ADB:-adb}"
        if ! "$ADB" get-state >/dev/null 2>&1; then echo "mobile: skipped (no adb device)"; exit 0; fi
        "$ADB" shell mkdir -p /data/local/tmp/eskiu >/dev/null
        run() {
            "$ADB" push -q "$1" /data/local/tmp/eskiu/t >/dev/null &&
            "$ADB" shell "cd /data/local/tmp/eskiu && chmod 755 t && ./t > o 2>/dev/null; true" &&
            "$ADB" exec-out cat /data/local/tmp/eskiu/o
        } ;;
    *) echo "error: PLATFORM must be ios or android" >&2; exit 2 ;;
esac
skip_list="$(dirname "$0")/skip.txt"
pass=0; fail=0; skipped=0
names=("$@")
if [[ ${#names[@]} -eq 0 ]]; then
    for e in "$here"/*.esk; do names+=("$(basename "$e" .esk)"); done
fi
for name in "${names[@]}"; do
    [[ -f "$here/$name.expected" && ! -f "$here/$name.c" ]] || continue
    if [[ -f "$skip_list" ]] && grep -q "^$name\b" "$skip_list"; then skipped=$((skipped+1)); continue; fi
    if ! "$ESKIUC" "$here/$name.esk" --target "$TARGET" -o "$work/$name" >"$work/cerr" 2>&1; then
        echo "  FAIL  $name: compile: $(grep -m1 -i error "$work/cerr")"
        fail=$((fail+1)); continue
    fi
    run "$work/$name" >"$work/out" 2>/dev/null
    if ! diff -q "$work/out" "$here/$name.expected" >/dev/null; then
        echo "  FAIL  $name: output differs"
        diff "$here/$name.expected" "$work/out" | head -4 | sed 's/^/        /'
        fail=$((fail+1)); continue
    fi
    pass=$((pass+1))
done
echo "mobile ($PLATFORM $TARGET): $pass passed, $fail failed, $skipped skipped"
[[ $fail -eq 0 ]]
