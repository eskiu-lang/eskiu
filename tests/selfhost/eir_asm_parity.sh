#!/usr/bin/env bash
# Byte parity of `--emit-asm` (the EIR ARM64 emitter) between the C++ compiler (eskiuc) and
# the self-hosted one (eskiuc-esk, selfhost/eir_asm.esk) over the run tests (tests/NAME.esk
# with a NAME.expected), for two targets: the host (Mach-O on macOS, ELF elsewhere) and
# aarch64-unknown-linux-gnu (ELF). The text does not depend on the host's architecture.
#
#   tests/selfhost/eir_asm_parity.sh [file.esk ...]
#
# The tests whose assembly is identical for both targets are listed in
# tests/selfhost/eir_asm_parity_pass.txt: one that stops matching fails the gate, one that
# starts matching is reported (UPDATE=1 rewrites the list). With files given, each is
# compared and its first difference shown.

set -u
export LC_ALL=C          # one sort order (and comm's) on every host
cd "$(dirname "$0")/../.." || exit 2

BIN="${ESKIUC:-}"
if [ -z "$BIN" ]; then
    if [ -x build/eskiuc ]; then BIN=build/eskiuc
    elif command -v eskiuc >/dev/null 2>&1; then BIN=eskiuc
    else echo "eir_asm_parity: cannot find eskiuc (set ESKIUC or build it)"; exit 2; fi
fi
SELF="${ESKIUC_ESK:-}"
if [ -z "$SELF" ]; then
    if [ -x build/eskiuc-esk ]; then SELF=build/eskiuc-esk
    else echo "eir_asm_parity: cannot find eskiuc-esk (set ESKIUC_ESK or build it)"; exit 2; fi
fi

# The runtime the assembly path compiles must be current in the self-host.
python3 tools/gen_eir_c_text.py --check || exit 1

work="$(mktemp -d -t eskiu_eirap.XXXXXX)"
trap 'rm -rf "$work"' EXIT

# same NAME FILE: 0 when both compilers write the same, non-empty assembly for each target.
same() {
    local n="$1" f="$2" tag
    for tag in host elf; do
        local tgt=()
        [ "$tag" = elf ] && tgt=(--target aarch64-unknown-linux-gnu)
        "$BIN" "$f" ${tgt[@]+"${tgt[@]}"} --emit-asm >"$work/$n.$tag.cpp" 2>/dev/null || return 1
        "$SELF" "$f" ${tgt[@]+"${tgt[@]}"} --emit-asm >"$work/$n.$tag.esk" 2>/dev/null || return 1
        [ -s "$work/$n.$tag.cpp" ] && cmp -s "$work/$n.$tag.cpp" "$work/$n.$tag.esk" || return 1
    done
    return 0
}

if [ $# -gt 0 ]; then
    fail=0
    for f in "$@"; do
        n="$(basename "$f" .esk)"
        if same "$n" "$f"; then
            echo "ok    $n"
        else
            echo "FAIL  $n"
            for tag in host elf; do
                if [ ! -s "$work/$n.$tag.cpp" ] || [ ! -f "$work/$n.$tag.esk" ]; then
                    echo "  ($tag) a compiler wrote no assembly"
                    break
                fi
                if ! cmp -s "$work/$n.$tag.cpp" "$work/$n.$tag.esk"; then
                    echo "  ($tag)"
                    diff "$work/$n.$tag.cpp" "$work/$n.$tag.esk" | head -n 20
                    break
                fi
            done
            fail=1
        fi
    done
    exit "$fail"
fi

list="tests/selfhost/eir_asm_parity_pass.txt"
matched=()
total=0
for f in tests/*.esk; do
    n="$(basename "$f" .esk)"
    [ -f "tests/$n.expected" ] || continue
    total=$((total + 1))
    same "$n" "$f" && matched+=("$n")
    rm -f "$work/$n".*
done

printf '%s\n' "${matched[@]+"${matched[@]}"}" | sed '/^$/d' | sort > "$work/now.txt"
if [ "${UPDATE:-0}" = 1 ]; then
    cp "$work/now.txt" "$list"
    echo "eir_asm_parity: wrote $list (${#matched[@]}/$total identical)"
    exit 0
fi
[ -f "$list" ] || : > "$list"
lost="$(comm -23 "$list" "$work/now.txt")"
gained="$(comm -13 "$list" "$work/now.txt")"
[ -n "$gained" ] && echo "eir_asm_parity: now identical (UPDATE=1 to record): $(echo $gained)"
echo "eir_asm_parity: ${#matched[@]}/$total identical"
if [ -n "$lost" ]; then
    echo "eir_asm_parity: FAILED, no longer identical: $(echo $lost)"
    exit 1
fi
exit 0
