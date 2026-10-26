#!/usr/bin/env bash
# Behavioral and structural parity gate for EIR generator in self-hosted compiler (selfhost/eir.esk).
#
# Compares `--test-eir` output from the C++ compiler (`eskiuc`) and the self-hosted
# compiler (`eskiuc-esk`).
#
# Usage: tests/selfhost/eir_parity.sh [file.esk ...]

set -u
cd "$(dirname "$0")/../.." || exit 2

BIN="${ESKIUC:-}"
if [ -z "$BIN" ]; then
    if [ -x build/eskiuc ]; then BIN=build/eskiuc
    elif command -v eskiuc >/dev/null 2>&1; then BIN=eskiuc
    else echo "eir_parity: cannot find eskiuc (set ESKIUC or build it)"; exit 2; fi
fi

SELF="${ESKIUC_ESK:-}"
if [ -z "$SELF" ]; then
    if [ -x build/eskiuc-esk ]; then SELF=build/eskiuc-esk
    else
        # Compile on the fly
        SELF="$(mktemp -t eskiuc_esk.XXXXXX)"
        trap 'rm -f "$SELF"' EXIT
        if ! "$BIN" selfhost/esk_main.esk -o "$SELF" >/dev/null 2>&1; then
            echo "eir_parity: failed to build selfhost/esk_main.esk"; exit 2
        fi
    fi
fi

fail=0
total=0

# 1. Byte-for-byte exact matches on key test cases
exact_tests=(tests/inline_asm.esk)
for f in "${exact_tests[@]}"; do
    total=$((total + 1))
    base="$(basename "$f" .esk)"
    cpp_eir="$("$BIN" "$f" --test-eir 2>/dev/null)"
    self_eir="$("$SELF" "$f" --test-eir 2>/dev/null)"
    if [ "$cpp_eir" = "$self_eir" ] && [ -n "$cpp_eir" ]; then
        echo "ok    $base  (exact EIR match)"
    else
        echo "FAIL  $base  (EIR mismatch)"
        diff -u <(echo "$cpp_eir") <(echo "$self_eir") | head -n 20
        fail=1
    fi
done

# 2. Volatile semantics validation
total=$((total + 1))
vol_eir="$("$SELF" tests/volatile_access.esk --test-eir 2>/dev/null)"
n_vol_load="$(grep -c 'load volatile' <<< "$vol_eir")"
n_vol_store="$(grep -c 'store volatile' <<< "$vol_eir")"
if [ "$n_vol_load" -ge 8 ] && [ "$n_vol_store" -ge 4 ]; then
    echo "ok    volatile_access  (EIR: $n_vol_load volatile loads, $n_vol_store volatile stores)"
else
    echo "FAIL  volatile_access  (EIR: expected volatile accesses, got $n_vol_load loads, $n_vol_store stores)"
    fail=1
fi

# 3. Bitfield explicit lowering validation
total=$((total + 1))
bf_eir="$("$SELF" tests/bitfields.esk --test-eir 2>/dev/null)"
n_offset_ptr="$(grep -c 'offset_ptr' <<< "$bf_eir")"
n_ashr="$(grep -c 'ashr' <<< "$bf_eir")"
n_or="$(grep -c 'or ' <<< "$bf_eir")"
if [ "$n_offset_ptr" -ge 10 ] && [ "$n_ashr" -ge 2 ] && [ "$n_or" -ge 3 ]; then
    echo "ok    bitfields  (EIR: $n_offset_ptr offset_ptrs, $n_ashr signed sign-extensions, $n_or mask combines)"
else
    echo "FAIL  bitfields  (EIR: missing bitfield lowering instructions)"
    fail=1
fi

# 4. Inline assembly with clobbers / constraints
total=$((total + 1))
asm_eir="$("$SELF" tests/inline_asm.esk --test-eir 2>/dev/null)"
if grep -q 'inline_asm "nop", ""' <<< "$asm_eir"; then
    echo "ok    inline_asm  (EIR: inline_asm instruction lowered)"
else
    echo "FAIL  inline_asm  (EIR: missing inline_asm instruction)"
    fail=1
fi

# 5. Short-circuit control flow (&& / ||)
total=$((total + 1))
sc_eir="$("$SELF" tests/short_circuit.esk --test-eir 2>/dev/null)"
n_sc_rhs="$(grep -c 'sc\.rhs' <<< "$sc_eir")"
n_sc_cont="$(grep -c 'sc\.cont' <<< "$sc_eir")"
if [ "$n_sc_rhs" -ge 3 ] && [ "$n_sc_cont" -ge 3 ]; then
    echo "ok    short_circuit  (EIR: $n_sc_rhs rhs blocks, $n_sc_cont continuation blocks)"
else
    echo "FAIL  short_circuit  (EIR: missing short-circuit basic blocks)"
    fail=1
fi

# 6. Pointer arithmetic scaling (gep)
total=$((total + 1))
ptr_eir="$("$SELF" tests/pointers.esk --test-eir 2>/dev/null)"
n_gep="$(grep -c 'gep ptr, ptr' <<< "$ptr_eir")"
if [ "$n_gep" -ge 3 ]; then
    echo "ok    pointers  (EIR: $n_gep scaled pointer geps)"
else
    echo "FAIL  pointers  (EIR: expected at least 3 pointer geps, got $n_gep)"
    fail=1
fi

echo "----"
if [ "$fail" -eq 0 ]; then
    echo "eir parity: $total/$total checks passed"
else
    echo "eir parity: FAILED"
fi
exit "$fail"
