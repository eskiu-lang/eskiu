#!/usr/bin/env bash
# cabi_parity.sh — the self-hosted back-end lowers by-value aggregates across the C ABI
# exactly like the C++ `eskiuc`, for every --target the C++ compiler lowers.
#
# For each C-ABI program and target triple this emits IR with BOTH compilers
# (--test-codegen --target T) and compares, by function name, the lowered signatures:
# every `declare` (externs) and every `@__cabi_*` callback thunk. Parameter names and
# the trailing `{` are stripped, so only types and ABI attributes (sret/byval/align/
# alignstack) are compared. Cross targets can't run here, so this is an IR-level check.
# For the 32-bit x86 and wasm32 targets each compiler's lowered `declare`s are also compared with
# the definitions clang emits for the program's C companion (NAME.c), when $CLANG can
# target them.
#
# Usage: tests/selfhost/cabi_parity.sh   (from repo root or anywhere)
set -u
export LC_ALL=C
cd "$(dirname "$0")/../.." || exit 2

BIN="${ESKIUC:-}"
if [ -z "$BIN" ]; then
    if [ -x build/eskiuc ]; then BIN=build/eskiuc
    elif command -v eskiuc >/dev/null 2>&1; then BIN=eskiuc
    else echo "cabi_parity: cannot find eskiuc (set ESKIUC or build it)"; exit 2; fi
fi
DRIVER=selfhost/esk_main.esk
ESKMAIN="$(mktemp -t esk_main.XXXXXX)"
WORK="$(mktemp -d)"
trap 'rm -f "$ESKMAIN"; rm -rf "$WORK"' EXIT

if ! ESKIU_ROOT="$(pwd)" "$BIN" "$DRIVER" -o "$ESKMAIN" >/dev/null 2>"$WORK/build.log"; then
    echo "cabi_parity: failed to build $DRIVER"; cat "$WORK/build.log"; exit 2
fi

TARGETS="arm64-apple-darwin aarch64-unknown-linux-gnu x86_64-unknown-linux-gnu
x86_64-apple-darwin x86_64-pc-windows-msvc x86_64-w64-windows-gnu
armv7-none-linux-gnueabihf armv6k-none-eabihf armv7-none-linux-gnueabi
i686-pc-linux-gnu i386-apple-darwin i686-w64-windows-gnu i686-pc-windows-msvc
wasm32-unknown-emscripten"
VS_CLANG="i686-pc-linux-gnu i386-apple-darwin i686-w64-windows-gnu i686-pc-windows-msvc
wasm32-unknown-emscripten"
INPUTS="tests/c_abi_struct.esk tests/c_abi_callback.esk tests/c_abi_va_callback.esk tests/c_abi_try.esk tests/c_abi_fnptr.esk tests/bitfield_c_layout.esk tests/c_abi_union.esk tests/c_abi_narrow.esk tests/c_abi_x86_32.esk tests/pack_nested_c.esk tests/pack_union_c.esk tests/bitfield_unnamed_c.esk"

# The lowered signatures in an IR file, one per line, sorted: `NAME<TAB>signature`. The C++
# names a union's storage type `%U.union` and the self-host `%U`; only the spelling differs.
sigs() {
    grep -E '^(declare .*@|define internal .*@__cabi_)' "$1" \
        | sed -E 's/ %[A-Za-z0-9_.]+([,)])/\1/g; s/ *\{$//; s/^define internal /define /; s/(%[A-Za-z0-9_]+)\.union/\1/g' \
        | sed -E "s/^([a-z]+ [^@]*@)([A-Za-z0-9_.]+)(\\(.*)$/\\2$TAB\\1\\2\\3/" \
        | sort
}

TAB="$(printf '\t')"
fail=0; n=0
for src in $INPUTS; do
    for t in $TARGETS; do
        n=$((n + 1))
        name="$(basename "$src" .esk)@$t"
        if ! ESKIU_ROOT="$(pwd)" "$BIN" "$src" --test-codegen --target "$t" >"$WORK/cpp.ll" 2>&1; then
            # A program the target cannot build (exceptions on WebAssembly) is skipped.
            if grep -q "not supported on WebAssembly" "$WORK/cpp.ll"; then echo "skip  $name"; continue; fi
            echo "FAIL  $name  (C++ codegen failed)"; fail=1; continue
        fi
        if ! ESKIU_ROOT="$(pwd)" "$ESKMAIN" "$src" --test-codegen --target "$t" >"$WORK/esk.ll" 2>"$WORK/esk.err"; then
            echo "FAIL  $name  (self-host codegen failed)"; head -3 "$WORK/esk.err"; fail=1; continue
        fi
        sigs "$WORK/cpp.ll" >"$WORK/cpp.sig"
        sigs "$WORK/esk.ll" >"$WORK/esk.sig"
        # A function both compilers declare (or thunk) must have the same lowered
        # signature, and both must emit the same set of thunks and the same EH personality.
        join -t "$TAB" "$WORK/cpp.sig" "$WORK/esk.sig" >"$WORK/both"
        diffs="$(awk -F"$TAB" '$2 != $3 { print $2 "  vs  " $3 }' "$WORK/both")"
        extra="$( (grep -E '@(__cabi_|__gxx_personality)' "$WORK/cpp.sig" | cut -f1; grep -E '@(__cabi_|__gxx_personality)' "$WORK/esk.sig" | cut -f1) | sort | uniq -u)"
        if [ -n "$diffs" ] || [ -n "$extra" ]; then
            echo "FAIL  $name"
            [ -n "$diffs" ] && echo "$diffs" | sed 's/^/  C++ vs self-host: /' | head -8
            [ -n "$extra" ] && echo "$extra" | sed 's/^/  thunk or personality in one compiler only: /' | head -8
            fail=1
        else
            echo "ok    $name  ($(wc -l <"$WORK/both" | tr -d ' ') signatures)"
        fi
    done
done
# 32-bit x86 and wasm32: the lowered extern signatures equal clang's definitions of the C side, by
# function name (clang's `noundef`/`dead_on_*`/`noalias`/`writable`/`dso_local`
# attributes and its `%struct.`/`%union.` prefixes are dropped).
CLANG="${CLANG:-clang}"
clang_sigs() {
    grep -E '^define .*@' "$1" \
        | sed -E 's/^define //; s/ #[0-9]+//; s/ *\{$//; s/(dso_local|hidden|noundef|dead_on_unwind|dead_on_return|noalias|writable|local_unnamed_addr) //g' \
        | sed -E 's/ %[A-Za-z0-9_.]+([,)])/\1/g; s/%(struct|union)\./%/g' \
        | sed -E "s/^([^@]*@)([A-Za-z0-9_.]+)(\\(.*)$/\\2$TAB\\1\\2\\3/" | sort
}
decl_sigs() {
    grep -E '^declare .*@' "$1" \
        | sed -E 's/^declare //; s/ %[A-Za-z0-9_.]+([,)])/\1/g; s/(%[A-Za-z0-9_]+)\.union/\1/g' \
        | sed -E "s/^([^@]*@)([A-Za-z0-9_.]+)(\\(.*)$/\\2$TAB\\1\\2\\3/" | sort
}
for src in $INPUTS; do
    csrc="${src%.esk}.c"
    [ -f "$csrc" ] || continue
    for t in $VS_CLANG; do
        name="$(basename "$src" .esk)@$t vs clang"
        if ! "$CLANG" -target "$t" -O0 -S -emit-llvm -o "$WORK/c.ll" "$csrc" 2>/dev/null; then
            echo "skip  $name  ($CLANG cannot target $t)"; continue
        fi
        clang_sigs "$WORK/c.ll" >"$WORK/c.sig"
        for who in C++ self-host; do
            n=$((n + 1))
            if [ "$who" = C++ ]; then ESKIU_ROOT="$(pwd)" "$BIN" "$src" --test-codegen --target "$t" >"$WORK/x.ll" 2>"$WORK/x.err"
            else ESKIU_ROOT="$(pwd)" "$ESKMAIN" "$src" --test-codegen --target "$t" >"$WORK/x.ll" 2>"$WORK/x.err"; fi
            if grep -q "not supported on WebAssembly" "$WORK/x.err"; then echo "skip  $name ($who)"; continue; fi
            decl_sigs "$WORK/x.ll" >"$WORK/x.sig"
            join -t "$TAB" "$WORK/x.sig" "$WORK/c.sig" >"$WORK/both"
            diffs="$(awk -F"$TAB" '$2 != $3 { print $2 "  vs clang  " $3 }' "$WORK/both")"
            if [ ! -s "$WORK/both" ] || [ -n "$diffs" ]; then
                echo "FAIL  $name ($who)"; echo "$diffs" | head -8 | sed 's/^/  /'; fail=1
            else
                echo "ok    $name ($who, $(wc -l <"$WORK/both" | tr -d ' ') signatures)"
            fi
        done
    done
done
# Type sizes follow each target's data layout (pointer width, i64/double alignment): the
# `@sz_*` globals of tests/target_sizes.esk fold to the same values in both compilers.
for t in $TARGETS; do
    n=$((n + 1))
    name="target_sizes@$t"
    ESKIU_ROOT="$(pwd)" "$BIN" tests/target_sizes.esk --test-codegen --target "$t" 2>&1 | grep '^@sz_' >"$WORK/cpp.sz"
    ESKIU_ROOT="$(pwd)" "$ESKMAIN" tests/target_sizes.esk --test-codegen --target "$t" 2>&1 | grep '^@sz_' >"$WORK/esk.sz"
    if [ ! -s "$WORK/cpp.sz" ] || ! cmp -s "$WORK/cpp.sz" "$WORK/esk.sz"; then
        echo "FAIL  $name"; diff "$WORK/cpp.sz" "$WORK/esk.sz" | head -8 | sed 's/^/  /'; fail=1
    else
        echo "ok    $name  ($(tr '\n' ' ' <"$WORK/cpp.sz" | sed -E 's/@sz_([a-z]+) = global i32 /\1=/g'))"
    fi
done
# Bitfield layouts that differ by target (tests/run_cmd/bitfield_targets.esk; run.sh checks
# the C++ sizes against clang's): the same `@sz_*`/`@al_*` in both compilers.
for t in $TARGETS; do
    n=$((n + 1))
    name="bitfield_targets@$t"
    ESKIU_ROOT="$(pwd)" "$BIN" tests/run_cmd/bitfield_targets.esk --test-codegen --target "$t" 2>&1 | grep -E '^@(sz|al)_' >"$WORK/cpp.sz"
    ESKIU_ROOT="$(pwd)" "$ESKMAIN" tests/run_cmd/bitfield_targets.esk --test-codegen --target "$t" 2>&1 | grep -E '^@(sz|al)_' >"$WORK/esk.sz"
    if [ ! -s "$WORK/cpp.sz" ] || ! cmp -s "$WORK/cpp.sz" "$WORK/esk.sz"; then
        echo "FAIL  $name"; diff "$WORK/cpp.sz" "$WORK/esk.sz" | head -8 | sed 's/^/  /'; fail=1
    else
        echo "ok    $name"
    fi
done
echo "----"
if [ "$fail" = 0 ]; then echo "cabi parity: $n program/target pairs match"; else echo "cabi parity: FAILED"; fi
exit "$fail"
