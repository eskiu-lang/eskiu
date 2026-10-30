#!/usr/bin/env bash
# Behavioral parity gate for the self-hosted code generator (selfhost/codegen.esk).
#
# LLVM auto-numbers SSA values and constant-folds, so the emitted .ll can't be
# matched byte-for-byte against `--test-codegen`. Instead the oracle is BEHAVIORAL:
# emit .ll → clang → run, and compare the exit code + stdout against the binary the
# C++ `eskiuc` produces from the same source. Same observable behavior = parity.
#
# Usage: tests/selfhost/cg_parity.sh [file.esk ...]
#   no args -> the synthetic corpus under tests/selfhost/cg_inputs/
# Green (exit 0) = identical exit code and stdout for every program.

set -u
cd "$(dirname "$0")/../.." || exit 2

BIN="${ESKIUC:-}"
if [ -z "$BIN" ]; then
    if [ -x build/eskiuc ]; then BIN=build/eskiuc
    elif command -v eskiuc >/dev/null 2>&1; then BIN=eskiuc
    else echo "cg_parity: cannot find eskiuc (set ESKIUC or build it)"; exit 2; fi
fi
CLANG="${CLANG:-clang}"   # CI installs clang as clang-22; override via $CLANG
command -v "$CLANG" >/dev/null 2>&1 || { echo "cg_parity: $CLANG not found (set CLANG)"; exit 2; }

DRIVER=selfhost/esk_main.esk
CGBIN="$(mktemp -t cg_main.XXXXXX)"
WORK="$(mktemp -d)"
trap 'rm -f "$CGBIN"; rm -rf "$WORK"' EXIT
if ! "$BIN" "$DRIVER" -o "$CGBIN" >/dev/null 2>"$WORK/build.log"; then
    echo "cg_parity: failed to build $DRIVER"; cat "$WORK/build.log"; exit 2
fi

if [ "$#" -gt 0 ]; then files=("$@"); else files=(tests/selfhost/cg_inputs/*.esk); fi

fail=0
total=0
for f in "${files[@]}"; do
    [ -f "$f" ] || { echo "MISS  $f"; fail=1; continue; }
    total=$((total + 1))
    base="$(basename "$f" .esk)"

    # Reference first: the C++ build. Link libc++abi so exception programs (which need
    # the Itanium __cxa_* runtime) link on both sides, matching the self-host clang
    # invocation below. An input the C++ rejects is skipped (the self-host type-checks
    # under --test-codegen too, so it would reject it as well).
    if ! "$BIN" "$f" -o "$WORK/$base.cpp" >/dev/null 2>&1; then
        echo "skip  $base  (C++ eskiuc could not build it)"; total=$((total - 1)); continue
    fi
    cpp_out="$("$WORK/$base.cpp" 2>/dev/null)"; cpp_code=$?

    # Self-hosted: emit .ll, compile with clang, run.
    if ! ESKIU_ROOT="$(pwd)" "$CGBIN" --test-codegen "$f" > "$WORK/$base.ll" 2>"$WORK/$base.emit.err"; then
        echo "FAIL  $base  (self-host codegen errored)"; sed 's/^/      /' "$WORK/$base.emit.err" | head; fail=1; continue
    fi
    if ! "$CLANG" "$WORK/$base.ll" -lc++abi -o "$WORK/$base.self" 2>"$WORK/$base.clang.err"; then
        echo "FAIL  $base  (clang rejected emitted .ll)"; sed 's/^/      /' "$WORK/$base.clang.err" | head; fail=1; continue
    fi
    self_out="$("$WORK/$base.self" 2>/dev/null)"; self_code=$?

    if [ "$self_code" = "$cpp_code" ] && [ "$self_out" = "$cpp_out" ]; then
        echo "ok    $base  (exit $self_code)"
    else
        echo "FAIL  $base  (self exit=$self_code out=$self_out | cpp exit=$cpp_code out=$cpp_out)"; fail=1
    fi
done

# Accesses through a `volatile` variable are volatile in the emitted IR (10 in the test's
# main, as the C++ back-end emits them; tests/run.sh checks the C++ side).
if [ "$#" -eq 0 ]; then
    total=$((total + 1))
    vn="$(ESKIU_ROOT="$(pwd)" "$CGBIN" --test-codegen tests/volatile_access.esk 2>/dev/null | grep -c 'volatile i32')"
    if [ "$vn" = 10 ]; then echo "ok    volatile_access  (IR: 10 volatile accesses)"
    else echo "FAIL  volatile_access  (IR: $vn volatile i32 accesses, expected 10)"; fail=1; fi
    total=$((total + 1))
    vb="$(ESKIU_ROOT="$(pwd)" "$CGBIN" --test-codegen tests/volatile_bitfield.esk 2>/dev/null | grep -c 'volatile i16')"
    if [ "$vb" = 12 ]; then echo "ok    volatile_bitfield  (IR: 12 volatile accesses)"
    else echo "FAIL  volatile_bitfield  (IR: $vb volatile i16 accesses, expected 12)"; fail=1; fi
    # -ffp-contract=fast: 4 contract ops in the test, none without the flag (tests/run.sh
    # checks the C++ side).
    total=$((total + 1))
    fc_on="$(ESKIU_ROOT="$(pwd)" "$CGBIN" --test-codegen -ffp-contract=fast tests/fp_contract.esk 2>/dev/null | grep -cE '(fmul|fadd|fsub) contract')"
    fc_off="$(ESKIU_ROOT="$(pwd)" "$CGBIN" --test-codegen tests/fp_contract.esk 2>/dev/null | grep -c ' contract ')"
    if [ "$fc_on" = 4 ] && [ "$fc_off" = 0 ]; then echo "ok    fp_contract  (IR: 4 contract ops)"
    else echo "FAIL  fp_contract  (IR: $fc_on contract ops with the flag, $fc_off without)"; fail=1; fi
    # C math externs that touch no memory are memory(none): 3 on Darwin, 1 on Linux
    # (tests/run.sh checks the C++ side).
    total=$((total + 1))
    lp_d="$(ESKIU_ROOT="$(pwd)" "$CGBIN" --test-codegen --target arm64-apple-macosx tests/run_cmd/libm_pure.esk 2>/dev/null | awk '/^attributes #[0-9]+ = .*memory\(none\)/ { p[$2] = 1 } { l[NR] = $0 } END { c = 0; for (i = 1; i <= NR; i++) if (l[i] ~ /^declare .*@(sqrt|floor|powf|cbrt)\(/) { if (l[i] ~ /memory\(none\)/) c++; else if (match(l[i], /#[0-9]+/) && p[substr(l[i], RSTART, RLENGTH)]) c++ } print c }')"
    lp_l="$(ESKIU_ROOT="$(pwd)" "$CGBIN" --test-codegen --target aarch64-unknown-linux-gnu tests/run_cmd/libm_pure.esk 2>/dev/null | awk '/^attributes #[0-9]+ = .*memory\(none\)/ { p[$2] = 1 } { l[NR] = $0 } END { c = 0; for (i = 1; i <= NR; i++) if (l[i] ~ /^declare .*@(sqrt|floor|powf|cbrt)\(/) { if (l[i] ~ /memory\(none\)/) c++; else if (match(l[i], /#[0-9]+/) && p[substr(l[i], RSTART, RLENGTH)]) c++ } print c }')"
    if [ "$lp_d" = 3 ] && [ "$lp_l" = 1 ]; then echo "ok    libm_pure  (IR: darwin 3, linux 1)"
    else echo "FAIL  libm_pure  (IR: darwin $lp_d, linux $lp_l; expected 3, 1)"; fail=1; fi
    # Fields of packed structs keep the alignment their address is known to have (14
    # accesses below natural alignment; tests/run.sh checks the C++ side).
    total=$((total + 1))
    pfa="$(ESKIU_ROOT="$(pwd)" "$CGBIN" --test-codegen tests/packed_field_align.esk 2>/dev/null | grep -E '(= load|store) (volatile )?(i16|i32|i64|double|float)[ ,]' | awk '{ t = ""; for (i = 1; i <= NF; i++) if ($i == "load" || $i == "store") { t = $(i + 1); if (t == "volatile") t = $(i + 2); break } sub(/,/, "", t); nat = (t == "i16") ? 2 : ((t == "i32" || t == "float") ? 4 : 8); al = nat; if (match($0, /align [0-9]+$/)) al = substr($0, RSTART + 6) + 0; if (al < nat) n++ } END { print n + 0 }')"
    if [ "$pfa" = 14 ]; then echo "ok    packed_field_align  (IR: 14 accesses below natural alignment)"
    else echo "FAIL  packed_field_align  (IR: $pfa accesses below natural alignment, expected 14)"; fail=1; fi
    # A lambda in a generic body keeps a heap env only where an instance lets it escape
    # (3 in the test's gl_ functions; tests/run.sh checks the C++ side).
    total=$((total + 1))
    gn="$(ESKIU_ROOT="$(pwd)" "$CGBIN" --test-codegen tests/generic_lambda_env.esk 2>/dev/null |
        awk '/^define/{fn=$0} /call ptr @malloc/{print fn}' | grep -c '@gl_')"
    if [ "$gn" = 3 ]; then echo "ok    generic_lambda_env  (IR: 3 heap envs)"
    else echo "FAIL  generic_lambda_env  (IR: $gn heap envs in gl_ functions, expected 3)"; fail=1; fi
    # Inline asm outputs lower like clang's on x86-64 (tests/run.sh checks the C++ side).
    total=$((total + 1))
    ia="$(ESKIU_ROOT="$(pwd)" "$CGBIN" --target x86_64-unknown-linux-gnu --test-codegen tests/inline_asm_out.esk 2>/dev/null)"
    if [[ "$ia" == *"call { i64, i64 } asm sideeffect"* && "$ia" == *'"=r,r,0,~{dirflag}'* \
       && "$ia" == *'"=&r,r,~{dirflag}'* && "$ia" == *'"=*m,*m,~{memory}'* \
       && "$(grep -c 'ptr elementtype(i64)' <<< "$ia")" -eq 2 ]]; then
        echo "ok    inline_asm_out  (x86-64 IR: outputs lowered like clang)"
    else echo "FAIL  inline_asm_out  (x86-64 asm output lowering)"; fail=1; fi
    total=$((total + 1))
    ig="$(ESKIU_ROOT="$(pwd)" "$CGBIN" --target x86_64-unknown-linux-gnu --test-codegen tests/inline_asm_gcc.esk 2>/dev/null)"
    if [[ "$ig" == *'"{ax},N{dx},~{dirflag}'* && "$ig" == *'"={ax},={bx},={cx},={dx},{ax},~{dirflag}'* \
       && "$ig" == *'"={ax},{cx},0,~{dirflag}'* && "$ig" == *'"={di},{si},~{dirflag}'* \
       && "$ig" == *'"=&{dx},{ax},~{dirflag}'* ]]; then
        echo "ok    inline_asm_gcc  (x86-64 IR: GCC register letters lowered like clang)"
    else echo "FAIL  inline_asm_gcc  (x86-64 GCC register constraints)"; fail=1; fi
    # An await the async lowering can't place is an error located at that await.
    total=$((total + 1))
    aw="$(ESKIU_ROOT="$(pwd)" "$CGBIN" --test-codegen tests/run_cmd/await_unplaced.esk 2>&1 >/dev/null)"
    case "$aw" in
        *"await_unplaced.esk:8:20: async function 'worker': 'await' is not supported here"*) echo "ok    await_unplaced  (located error)" ;;
        *) echo "FAIL  await_unplaced  ($aw)"; fail=1 ;;
    esac
fi

echo "----"
if [ "$fail" -eq 0 ]; then echo "cg parity: $total/$total programs match"; else echo "cg parity: MISMATCH"; fi
exit "$fail"
