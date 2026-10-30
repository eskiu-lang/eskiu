#!/usr/bin/env bash
#
# Eskiu test runner.
#
# Three kinds of tests, all driven off the files in this directory:
#
#   1. run    tests/NAME.esk + tests/NAME.expected
#             Compile -> link -> execute, then require stdout to match
#             NAME.expected exactly. A regression that changes output FAILS.
#
#   2. smoke  tests/NAME.esk with NO NAME.expected
#             Compile -> link -> execute, require exit code 0 only.
#             Used for tests whose output is non-deterministic (threads) or
#             which only need to prove they build and run.
#
#   A run or smoke test may have a C companion, tests/NAME.c: it is compiled with
#   $CC and linked into the test binary (used to check calls across the C ABI).
#   eskiuc links each test itself with no -l flags, so the libraries a program
#   implies (#pragma link, the C++ exception runtime, pthread) are proven here.
#
#   3. error  tests/errors/NAME.esk
#             Run --test-typechecker; require a NON-zero exit AND that the
#             diagnostics contain the substring after "EXPECT-ERROR:" on the
#             file's first line. Proves the compiler REJECTS bad code.
#
# Usage:  tests/run.sh            (from repo root or anywhere)
#         ESKIUC=/path/eskiuc tests/run.sh
#
# Every test runs on the machine alone (servers on 127.0.0.1) except the ones in
# RESOLVER_TESTS, which also need the system resolver to map localhost to loopback and
# fail for .invalid names. A proxy that answers every name (fake-ip, 198.18.0.0/15)
# breaks them: ESKIU_RESOLVER_TESTS=0 skips them.
#
set -u

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
ESKIUC="${ESKIUC:-$root/build/eskiuc}"
CC="${CC:-clang}"
export CC                      # eskiuc links with $CC
# C++ driver for the unit tests: derive from CC (clang-22 → clang++-22) unless set.
CXX="${CXX:-${CC/clang/clang++}}"

# Hardening gate: SANITIZE=asan|ubsan compiles every positive test with the
# matching instrumentation and runs it; a sanitizer abort fails the test.
# ubsan traps (no runtime); asan links its runtime (eskiuc --asan does that).
SANITIZE="${SANITIZE:-}"
SAN_FLAG=""
case "$SANITIZE" in
    # detect_leaks=0: the gate targets memory *corruption* (overflow / use-after-
    # free — the alloca-bug class), not leaks. Linux asan turns on LeakSanitizer
    # by default, and the test programs are short-lived and not leak-audited, so
    # leak detection stays off here (auditing the suite for leaks is future work).
    asan)  SAN_FLAG="--asan"
           export ASAN_OPTIONS="detect_leaks=0" ;;
    ubsan) SAN_FLAG="--ubsan" ;;
    "")    ;;
    *)     echo "error: SANITIZE must be asan, ubsan, or unset" >&2; exit 2 ;;
esac
[[ -n "$SANITIZE" ]] && echo "(sanitizer: $SANITIZE)"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

pass=0
fail=0
failed_names=()

if [[ ! -x "$ESKIUC" ]]; then
    echo "error: compiler not found at $ESKIUC (build it: cmake --build build)" >&2
    exit 2
fi

RESOLVER_TESTS=" dns_connect fetch_dns "
ESKIU_RESOLVER_TESTS="${ESKIU_RESOLVER_TESTS:-1}"

ok()   { printf '  \033[32mPASS\033[0m  %s\n' "$1"; pass=$((pass+1)); }
bad()  { printf '  \033[31mFAIL\033[0m  %s — %s\n' "$1" "$2"; fail=$((fail+1)); failed_names+=("$1"); }

# ---- positive tests (run + smoke) -----------------------------------------
echo "Positive tests:"
for esk in "$here"/*.esk; do
    name="$(basename "$esk" .esk)"
    bin="$work/$name"
    if [[ "$ESKIU_RESOLVER_TESTS" == 0 && "$RESOLVER_TESTS" == *" $name "* ]]; then
        printf '  SKIP  %s (ESKIU_RESOLVER_TESTS=0)\n' "$name"
        continue
    fi

    companion=()
    if [[ -f "$here/$name.c" ]]; then
        if ! $CC -c "$here/$name.c" -o "$work/$name.c.o" >"$work/lerr" 2>&1; then
            bad "$name" "C companion failed to compile: $(head -1 "$work/lerr")"
            continue
        fi
        companion=(--link-arg "$work/$name.c.o")
    fi
    if ! "$ESKIUC" $SAN_FLAG "$esk" ${companion[@]+"${companion[@]}"} -o "$bin" >"$work/cerr" 2>&1; then
        bad "$name" "compile/link failed: $(grep -m1 -v 'built for newer' "$work/cerr")"
        continue
    fi
    "$bin" >"$work/out" 2>&1
    code=$?

    expected="$here/$name.expected"
    if [[ -f "$expected" ]]; then
        if [[ $code -ne 0 ]]; then
            bad "$name" "exited $code (expected 0)"
        elif diff -u "$expected" "$work/out" >"$work/diff" 2>&1; then
            ok "$name"
        else
            bad "$name" "output mismatch"
            sed 's/^/        /' "$work/diff"
        fi
    else
        # Smoke test (non-deterministic output, e.g. threads/timing): exit 0 is enough.
        # (The http2 tests once flaked with a Linux-only SIGILL from an uninitialized
        # fn-pointer; that class was fixed at the root when `alloc<T>` began zero-initializing,
        # so the earlier retry mitigation is gone and a crash now reds the build directly.)
        if [[ $code -eq 0 ]]; then
            ok "$name (smoke)"
        else
            bad "$name" "exited $code (smoke test expects 0)"
        fi
    fi
done

# ---- negative tests (must be rejected) ------------------------------------
echo "Negative tests (must fail to type-check):"
if [[ -d "$here/errors" ]]; then
    for esk in "$here"/errors/*.esk; do
        [[ -e "$esk" ]] || continue
        name="errors/$(basename "$esk" .esk)"
        want="$(grep -m1 'EXPECT-ERROR:' "$esk" | sed 's/.*EXPECT-ERROR:[[:space:]]*//')"

        "$ESKIUC" "$esk" --test-typechecker >"$work/out" 2>&1
        code=$?
        if [[ $code -eq 0 ]]; then
            bad "$name" "compiler ACCEPTED code that should be rejected"
        elif [[ -n "$want" ]] && ! grep -qF "$want" "$work/out"; then
            bad "$name" "rejected, but message missing expected text: \"$want\""
        else
            ok "$name"
        fi
    done
fi

# ---- deep input (generated) -----------------------------------------------
# tests/deep/gen.sh writes programs too large to check in: 100000-operand operator
# chains and 10000-deep nesting must compile and run; nesting past the parser's limit
# must be a clean error, not a stack overflow.
echo "Deep input (generated by tests/deep/gen.sh):"
deep="$work/deep"
if bash "$here/deep/gen.sh" "$deep"; then
    for esk in "$deep"/*.esk; do
        name="deep/$(basename "$esk" .esk)"
        want="$(head -1 "$esk" | grep 'EXPECT-ERROR:' | sed 's/.*EXPECT-ERROR:[[:space:]]*//')"
        if [[ -n "$want" ]]; then
            "$ESKIUC" "$esk" --test-typechecker >"$work/out" 2>&1
            code=$?
            if [[ $code -eq 0 ]]; then bad "$name" "compiler ACCEPTED code that should be rejected"
            elif [[ $code -gt 1 ]]; then bad "$name" "compiler crashed (exit $code)"
            elif ! grep -qF "$want" "$work/out"; then bad "$name" "rejected, but message missing expected text: \"$want\""
            else ok "$name"; fi
            continue
        fi
        bin="$work/deep_$(basename "$esk" .esk)"
        if ! "$ESKIUC" $SAN_FLAG "$esk" -o "$bin" >"$work/cerr" 2>&1; then
            bad "$name" "compile/link failed: $(grep -m1 -v 'built for newer' "$work/cerr")"; continue
        fi
        if "$bin" >"$work/out" 2>&1 && diff -q "${esk%.esk}.expected" "$work/out" >/dev/null; then
            ok "$name"
        else
            bad "$name" "output mismatch or non-zero exit"
        fi
    done
else
    bad "deep/gen" "tests/deep/gen.sh failed"
fi

# ---- lint tests (-Wall) ----------------------------------------------------
# tests/warnings/NAME.esk must type-check under -Wall and emit exactly the warnings
# listed on its `// EXPECT-WARNING: <substring>` lines (none listed = none allowed),
# so a false-positive lint fails the suite.
echo "Lint tests (-Wall):"
if [[ -d "$here/warnings" ]]; then
    for esk in "$here"/warnings/*.esk; do
        [[ -e "$esk" ]] || continue
        name="warnings/$(basename "$esk" .esk)"
        if ! "$ESKIUC" -Wall "$esk" --test-typechecker >"$work/out" 2>&1; then
            bad "$name" "rejected: $(grep -m1 'error' "$work/out")"; continue
        fi
        got="$(grep -c 'warning:' "$work/out")"
        want_n="$(grep -c 'EXPECT-WARNING:' "$esk")"
        missing=""
        while IFS= read -r w; do
            [[ -n "$w" ]] && ! grep -qF "$w" "$work/out" && missing="$w"
        done < <(grep 'EXPECT-WARNING:' "$esk" | sed 's/.*EXPECT-WARNING:[[:space:]]*//')
        if [[ -n "$missing" ]]; then bad "$name" "missing warning \"$missing\""
        elif [[ "$got" -ne "$want_n" ]]; then bad "$name" "$got warning(s), expected $want_n: $(grep -m1 'warning:' "$work/out")"
        else ok "$name"; fi
    done
fi

# ---- single-file test modes ------------------------------------------------
# --test-lexer/--test-parser/--test-typechecker/--test-codegen must see the same
# program a real build does (same predefined macros), and exit 0 on success. A
# positive test marked `// TEST-MODES` on its first line runs through each mode.
echo "Test modes (same preprocessing as a build):"
for esk in "$here"/*.esk; do
    head -1 "$esk" | grep -q 'TEST-MODES' || continue
    name="$(basename "$esk" .esk)"
    for mode in --test-lexer --test-parser --test-typechecker --test-codegen; do
        if "$ESKIUC" "$esk" $mode >"$work/out" 2>&1; then
            ok "$mode/$name"
        else
            bad "$mode/$name" "exited non-zero: $(grep -m1 error "$work/out")"
        fi
    done
done

# ---- eskiuc run -------------------------------------------------------------
# `run [flags] script [--] args`: a flag's separate value is not the script, a `--`
# after the script is dropped, the program's exit code is propagated, and a
# program killed by a signal exits 128+signal like a shell.
echo "run subcommand:"
args_esk="$here/run_cmd/args.esk"
out="$("$ESKIUC" run -o "$work/unused" "$args_esk" -- a "b c" 2>/dev/null)"; code=$?
if [[ $code -eq 3 && "$out" == $'[a]\n[b c]' ]]; then ok "run/args"; else bad "run/args" "exit $code, output '$out'"; fi
"$ESKIUC" run "$args_esk" crash >/dev/null 2>&1; code=$?
if [[ $code -eq 139 ]]; then ok "run/signal"; else bad "run/signal" "exit $code (expected 139)"; fi

# ---- eskiuc repl -------------------------------------------------------------
# tests/repl/NAME.in is fed to `eskiuc repl` on stdin (no prompts: not a TTY), from
# tests/repl so `:load` finds its files; stdout and stderr must match NAME.expected.
# Not under a sanitizer: the REPL JIT-compiles without one.
if [[ -z "$SANITIZE" ]]; then
    echo "REPL:"
    for in_file in "$here"/repl/*.in; do
        name="$(basename "$in_file" .in)"
        (cd "$here/repl" && "$ESKIUC" repl <"$in_file" >"$work/repl_out" 2>&1)
        if diff -q "$work/repl_out" "$here/repl/$name.expected" >/dev/null 2>&1; then
            ok "repl/$name"
        else
            bad "repl/$name" "transcript differs: $(diff "$here/repl/$name.expected" "$work/repl_out" | head -4 | tr '\n' ' ')"
        fi
    done
fi

# Examples with a tests/examples/NAME.expected: examples/NAME.esk is built and run,
# and its output compared. fork() does not exist on Windows (fork.esk says so).
echo "Examples:"
for exp in "$here"/examples/*.expected; do
    [[ -e "$exp" ]] || continue
    name="$(basename "$exp" .expected)"
    case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) [[ "$name" == fork ]] && continue ;; esac
    if ! "$ESKIUC" $SAN_FLAG "$here/../examples/$name.esk" -o "$work/ex_$name" > "$work/ex_$name.log" 2>&1; then
        bad "examples/$name" "build failed: $(head -2 "$work/ex_$name.log" | tr '\n' ' ')"
    elif "$work/ex_$name" > "$work/ex_$name.out" 2>&1 && diff -q "$work/ex_$name.out" "$exp" > /dev/null; then
        ok "examples/$name"
    else
        bad "examples/$name" "output differs: $(diff "$exp" "$work/ex_$name.out" | head -4 | tr '\n' ' ')"
    fi
done

# The example scripts in examples/scripts/, run through their shebang line against a
# local fake API and a temporary directory tree (tests/scripts/check.sh). Not under a
# sanitizer: they run through `eskiuc run`, which builds without one.
if [[ -z "$SANITIZE" ]]; then
    echo "Example scripts:"
    while IFS= read -r line; do
        case "$line" in
            "ok "*)   ok "scripts/${line#ok }" ;;
            "FAIL "*) bad "scripts/${line#FAIL }" "transcript differs from tests/scripts/*.expected" ;;
            *)        echo "$line" ;;
        esac
    done < <(ESKIUC="$ESKIUC" bash "$here/scripts/check.sh" 2>&1)
fi

# The package manager (tests/pkg/check.sh): new/add/build/run/test/fetch against local
# git repositories, package imports in both compilers. Skipped without git.
if [[ -z "$SANITIZE" ]]; then
    echo "Packages:"
    while IFS= read -r line; do
        case "$line" in
            "ok "*)   ok "pkg/${line#ok }" ;;
            "FAIL "*) rest="${line#FAIL }"; bad "pkg/${rest%%:*}" "${rest#*: }" ;;
            *)        echo "  $line" ;;
        esac
    done < <(ESKIUC="$ESKIUC" bash "$here/pkg/check.sh" 2>&1)
fi

# $CC may carry arguments; a $CC that does not exist warns and falls back.
if CC="$CC -O0" "$ESKIUC" "$args_esk" -o "$work/cc_args" >/dev/null 2>&1 && [[ -x "$work/cc_args" ]]; then
    ok "cli/cc-with-args"
else
    bad "cli/cc-with-args" "CC with arguments did not link"
fi
cc_out="$(CC=no-such-cc-driver "$ESKIUC" "$args_esk" -o "$work/cc_bad" 2>&1)"
if [[ -x "$work/cc_bad" && "$cc_out" == *"warning: \$CC"* ]]; then
    ok "cli/cc-missing"
else
    bad "cli/cc-missing" "no fallback warning: $cc_out"
fi

# The libraries a program implies are added once, after the objects, only when
# linking an executable, and not at all under --no-default-libs. A $CC wrapper
# records the link line.
cat > "$work/cc_log.sh" <<EOF
#!/bin/sh
echo "\$@" >> "$work/cc_log"
exec $CC "\$@"
EOF
chmod +x "$work/cc_log.sh"
linkline() { rm -f "$work/cc_log"; CC="$work/cc_log.sh" "$ESKIUC" "$@" >/dev/null 2>&1; cat "$work/cc_log" 2>/dev/null; }
ll="$(linkline "$here/pragma_link.esk" -o "$work/pl" -lm)"
if [[ "$ll" == *" -lm"* && "$ll" != *" -lm"*" -lm"* && "$ll" != *eskiu_no_such_lib* ]]; then ok "cli/link-pragma-dedup"
else bad "cli/link-pragma-dedup" "link line: $ll"; fi
ll="$(linkline "$here/exceptions.esk" -o "$work/ex_nd" --no-default-libs)"
if [[ -n "$ll" && "$ll" != *" -l"* ]]; then ok "cli/no-default-libs"
else bad "cli/no-default-libs" "link line: $ll"; fi
ll="$(linkline --target x86_64-unknown-linux-gnu "$here/exceptions.esk" -o "$work/ex_lx")"
if [[ "$ll" == *" -lstdc++"* ]]; then ok "cli/eh-runtime-linux"
else bad "cli/eh-runtime-linux" "link line: $ll"; fi
ll="$(linkline --target aarch64-none-elf "$here/exceptions.esk" -o "$work/ex_none")"
if [[ -n "$ll" && "$ll" != *" -l"* ]]; then ok "cli/bare-metal-no-libs"
else bad "cli/bare-metal-no-libs" "link line: $ll"; fi
printf '#pragma link("eskiu_no_such_lib")\nint main() { return 0; }\n' > "$work/badlib.esk"
if ! "$ESKIUC" "$work/badlib.esk" -o "$work/badlib" >/dev/null 2>&1 \
   && "$ESKIUC" "$work/badlib.esk" -c -o "$work/badlib.o" >/dev/null 2>&1 \
   && "$ESKIUC" "$work/badlib.esk" --no-default-libs -o "$work/badlib" >/dev/null 2>&1; then
    ok "cli/link-pragma-applied"
else
    bad "cli/link-pragma-applied" "a missing #pragma link library did not fail only the link"
fi

# Every access through a `volatile` variable is a volatile load/store (10 in the test).
v_n="$("$ESKIUC" --test-codegen "$here/volatile_access.esk" 2>/dev/null | grep -c 'volatile i32')"
if [[ "$v_n" -eq 10 ]]; then ok "codegen/volatile-access"
else bad "codegen/volatile-access" "$v_n volatile i32 accesses in the IR, expected 10"; fi
# -ffp-contract=fast marks the multiplies, adds and subtracts `contract` (4 in the test);
# without it none is.
fc_on="$("$ESKIUC" --test-codegen -ffp-contract=fast "$here/fp_contract.esk" 2>/dev/null | grep -cE '(fmul|fadd|fsub) contract')"
fc_off="$("$ESKIUC" --test-codegen "$here/fp_contract.esk" 2>/dev/null | grep -c ' contract ')"
if [[ "$fc_on" -eq 4 && "$fc_off" -eq 0 ]]; then ok "codegen/fp-contract"
else bad "codegen/fp-contract" "$fc_on contract ops with the flag (want 4), $fc_off without (want 0)"; fi
# -fstrict-aliasing tags the typed scalar accesses (none without the flag), and the test
# keeps its output at -O2 with it: unions, pointer casts and bytes still alias.
sa_on="$("$ESKIUC" --test-codegen -fstrict-aliasing "$here/strict_aliasing.esk" 2>/dev/null | grep -c '!tbaa')"
sa_off="$("$ESKIUC" --test-codegen "$here/strict_aliasing.esk" 2>/dev/null | grep -c '!tbaa')"
if [[ "$sa_on" -gt 0 && "$sa_off" -eq 0 ]] \
   && "$ESKIUC" "$here/strict_aliasing.esk" -O2 -fstrict-aliasing -o "$work/sa" >/dev/null 2>&1 \
   && "$work/sa" 2>&1 | diff -q - "$here/strict_aliasing.expected" >/dev/null; then
    ok "codegen/strict-aliasing"
else
    bad "codegen/strict-aliasing" "$sa_on tagged accesses with the flag, $sa_off without, or a wrong -O2 result"
fi
# A lambda in a generic body keeps a heap env only where an instance lets it escape (3).
gl_n="$("$ESKIUC" --test-codegen "$here/generic_lambda_env.esk" 2>/dev/null |
    awk '/^define/{fn=$0} /env.heap = /{print fn}' | grep -c '@gl_')"
if [[ "$gl_n" -eq 3 ]]; then ok "codegen/generic-lambda-env"
else bad "codegen/generic-lambda-env" "$gl_n heap envs in the gl_ functions, expected 3"; fi
# A bitfield through a `volatile` variable, and a volatile variable's initializing store.
vb_n="$("$ESKIUC" --test-codegen "$here/volatile_bitfield.esk" 2>/dev/null | grep -c 'volatile i16')"
if [[ "$vb_n" -eq 12 ]]; then ok "codegen/volatile-bitfield"
else bad "codegen/volatile-bitfield" "$vb_n volatile i16 accesses in the IR, expected 12"; fi

# Inline asm outputs lower like clang's on x86-64: two register outputs are a struct
# result, `+r` ties an input to its output, `=m`/`+m` are indirect `ptr elementtype`.
ia="$("$ESKIUC" --target x86_64-unknown-linux-gnu --test-codegen "$here/inline_asm_out.esk" 2>/dev/null)"
if [[ "$ia" == *"call { i64, i64 } asm sideeffect"* && "$ia" == *'"=r,r,0,~{dirflag}'* \
   && "$ia" == *'"=&r,r,~{dirflag}'* && "$ia" == *'"=*m,*m,~{memory}'* \
   && "$(grep -c 'ptr elementtype(i64)' <<< "$ia")" -eq 2 ]]; then
    ok "codegen/inline-asm-outputs-x86-64"
else
    bad "codegen/inline-asm-outputs-x86-64" "asm output constraints or operands differ from clang's lowering"
fi
# GCC register letters on x86 become LLVM's explicit registers, as clang spells them.
ig="$("$ESKIUC" --target x86_64-unknown-linux-gnu --test-codegen "$here/inline_asm_gcc.esk" 2>/dev/null)"
if [[ "$ig" == *'"{ax},N{dx},~{dirflag}'* && "$ig" == *'"={ax},={bx},={cx},={dx},{ax},~{dirflag}'* \
   && "$ig" == *'"={ax},{cx},0,~{dirflag}'* && "$ig" == *'"={di},{si},~{dirflag}'* \
   && "$ig" == *'"=&{dx},{ax},~{dirflag}'* ]]; then
    ok "codegen/inline-asm-gcc-registers-x86-64"
else
    bad "codegen/inline-asm-gcc-registers-x86-64" "GCC register constraints not lowered like clang's"
fi

# <net>'s struct timeval is two C `long`s: 8 bytes on 32-bit ARM, 16 on 64-bit.
tv_arm="$("$ESKIUC" --target armv7-unknown-linux-gnueabihf --test-codegen "$here/run_cmd/net_timeval.esk" 2>/dev/null)"
tv_x64="$("$ESKIUC" --target x86_64-unknown-linux-gnu --test-codegen "$here/run_cmd/net_timeval.esk" 2>/dev/null)"
if [[ "$tv_arm" == *"%_net_timeval = type { i32, i32 }"* && "$tv_arm" == *"ptr %tv, i32 8)"* \
   && "$tv_x64" == *"%_net_timeval = type { i64, i64 }"* && "$tv_x64" == *"ptr %tv, i32 16)"* ]]; then
    ok "codegen/net-timeval-width"
else
    bad "codegen/net-timeval-width" "timeval layout or optlen wrong for armv7 or x86-64"
fi

# Bitfield layouts that differ by target (an int64 bitfield on 32-bit x86 SysV, unnamed
# bitfields): the sizes and alignments clang gives each target.
bt_fail=""
while read -r bt_t bt_want; do
    bt_got="$("$ESKIUC" --target "$bt_t" --test-codegen "$here/run_cmd/bitfield_targets.esk" 2>/dev/null \
        | sed -nE 's/^@((sz|al)_[a-z0-9]+) = .*global i32 ([0-9]+).*/\1=\3/p' | tr '\n' ' ')"
    [[ "$bt_got" == "$bt_want " ]] || bt_fail="$bt_fail $bt_t: $bt_got;"
done <<'EOF'
x86_64-unknown-linux-gnu sz_l1=8 sz_l2=8 sz_l3=16 sz_u1=5 sz_u2=3 sz_u3=2 sz_u4=5 al_l3=8 al_u2=1 al_u3=1
i686-pc-linux-gnu sz_l1=4 sz_l2=8 sz_l3=12 sz_u1=5 sz_u2=3 sz_u3=2 sz_u4=5 al_l3=4 al_u2=1 al_u3=1
aarch64-unknown-linux-gnu sz_l1=8 sz_l2=8 sz_l3=16 sz_u1=8 sz_u2=4 sz_u3=4 sz_u4=8 al_l3=8 al_u2=4 al_u3=4
armv7-none-linux-gnueabihf sz_l1=8 sz_l2=8 sz_l3=16 sz_u1=8 sz_u2=4 sz_u3=4 sz_u4=8 al_l3=8 al_u2=4 al_u3=4
x86_64-w64-windows-gnu sz_l1=8 sz_l2=16 sz_l3=16 sz_u1=2 sz_u2=12 sz_u3=4 sz_u4=8 al_l3=8 al_u2=4 al_u3=1
arm64-apple-darwin sz_l1=8 sz_l2=8 sz_l3=16 sz_u1=5 sz_u2=3 sz_u3=2 sz_u4=5 al_l3=8 al_u2=1 al_u3=1
EOF
if [[ -z "$bt_fail" ]]; then ok "codegen/bitfield-target-layouts"
else bad "codegen/bitfield-target-layouts" "sizes differ from clang's:$bt_fail"; fi

# An await the async lowering cannot place is an error located at the await (an asm
# input).
aw_out="$("$ESKIUC" "$here/run_cmd/await_unplaced.esk" -o "$work/await_unplaced" 2>&1)"
if [[ "$aw_out" == *"await_unplaced.esk:8:20: async function 'worker': 'await' is not supported here"* ]]; then
    ok "cli/await-unplaced-located"
else
    bad "cli/await-unplaced-located" "$(printf '%s' "$aw_out" | grep -m1 error)"
fi

# A script compiled with another input that defines main, or that has statements too, is
# one located error (each file alone is checked by its parser).
sm_out="$("$ESKIUC" "$here/run_cmd/script_with_main_b.esk" "$here/run_cmd/script_with_main_a.esk" -o "$work/script_with_main" 2>&1)"
sm_two="$("$ESKIUC" "$here/run_cmd/script_with_main_a.esk" "$here/script_toplevel.esk" -o "$work/script_with_main" 2>&1)"
if [[ "$sm_out" == *"script_with_main_a.esk:3:1: a file with top-level statements has no 'main'"* &&
      "$sm_two" == *"script_toplevel.esk:7:1: only one input file may have top-level statements"* ]]; then
    ok "cli/script-with-main-input"
else
    bad "cli/script-with-main-input" "$(printf '%s %s' "$sm_out" "$sm_two" | grep -m1 error)"
fi

# <process>: a child with a timeout reads an inherited terminal stdin (run under a
# pseudo-terminal by `script`, skipped where there is none).
if command -v script >/dev/null 2>&1 && "$ESKIUC" "$here/run_cmd/process_tty.esk" -o "$work/process_tty" >/dev/null 2>&1; then
    if [[ "$(uname)" == "Darwin" ]]; then
        tty_out="$( (sleep 1; printf 'hello\n'; sleep 1) | script -q /dev/null "$work/process_tty" 2>&1)"
    else
        tty_out="$( (sleep 1; printf 'hello\n'; sleep 1) | script -qec "$work/process_tty" /dev/null 2>&1)"
    fi
    if [[ "$tty_out" == *"code=0 timed_out=0 fast=1 out=got:hello"* ]]; then
        ok "stdlib/process-timeout-tty-stdin"
    else
        bad "stdlib/process-timeout-tty-stdin" "$(printf '%s' "$tty_out" | tr '\r\n' '  ')"
    fi
fi

# -Werror turns the default-on warnings into errors (nonzero exit), -w silences them.
we_file="$here/warnings/string_compare.esk"
"$ESKIUC" -Werror --test-typechecker "$we_file" > "$work/we.out" 2>&1; we_code=$?
"$ESKIUC" -w --test-typechecker "$we_file" > "$work/wn.out" 2>&1; wn_code=$?
if [[ $we_code -ne 0 ]] && grep -q "^error: .*\[-Wstring-compare\]" "$work/we.out" \
   && [[ $wn_code -eq 0 ]] && ! grep -q "warning:" "$work/wn.out"; then
    ok "cli/werror-and-w"
else
    bad "cli/werror-and-w" "-Werror exit $we_code, -w exit $wn_code: $(grep -m2 -E 'error|warning' "$work/we.out" "$work/wn.out" | tr '\n' ' ')"
fi

# -Wno-unused-result / -Wno-await-holding-lock silence those default-on warnings.
wno_out="$("$ESKIUC" -Wno-unused-result --test-typechecker "$here/warnings/unused_result.esk" 2>&1; "$ESKIUC" -Wno-await-holding-lock --test-typechecker "$here/warnings/await_holding_lock.esk" 2>&1)"
wyes_out="$("$ESKIUC" --test-typechecker "$here/warnings/await_holding_lock.esk" 2>&1)"
if ! grep -q "warning:" <<<"$wno_out" && grep -q "\[-Wawait-holding-lock\]" <<<"$wyes_out"; then
    ok "cli/wno-unused-result-await-lock"
else
    bad "cli/wno-unused-result-await-lock" "$(grep -m2 warning <<<"$wno_out" | tr '\n' ' ')"
fi

# A successful compile prints nothing to stdout (no output-file echo).
printf 'int main() { return 0; }\n' > "$work/quiet.esk"
q_out="$("$ESKIUC" "$work/quiet.esk" -o "$work/quiet" 2>/dev/null)"
q_obj="$("$ESKIUC" "$work/quiet.esk" -c -o "$work/quiet.o" 2>/dev/null)"
if [[ -z "$q_out" && -z "$q_obj" && -x "$work/quiet" ]]; then
    ok "cli/quiet-success"
else
    bad "cli/quiet-success" "stdout: $q_out $q_obj"
fi

# -Wall points an unused-parameter warning at the parameter itself.
w_out="$("$ESKIUC" -Wall --test-typechecker "$here/run_cmd/unused_param.esk" 2>&1)"
if [[ "$w_out" == *"unused_param.esk:4:11: warning: unused parameter 'unused_p'"* ]]; then
    ok "cli/unused-param-position"
else
    bad "cli/unused-param-position" "$(printf '%s' "$w_out" | grep -m1 warning)"
fi

# The test modes take every input, like a build: a #define in the first reaches the
# later ones, and an error in a second input fails --test-typechecker.
printf '#define LIB_VAL 5\n' > "$work/mf_a.esk"
printf 'int lib() { return LIB_VAL; }\n' > "$work/mf_b.esk"
printf 'int main() { return lib() - 5; }\n' > "$work/mf_c.esk"
printf 'int main() { return 0; }\n' > "$work/mf_ok.esk"
printf 'int bad() { return nope; }\n' > "$work/mf_bad.esk"
if "$ESKIUC" --test-typechecker "$work/mf_a.esk" "$work/mf_b.esk" "$work/mf_c.esk" >/dev/null 2>&1 \
   && "$ESKIUC" --test-codegen "$work/mf_a.esk" "$work/mf_b.esk" "$work/mf_c.esk" 2>/dev/null | grep -q 'define.*@lib' \
   && ! "$ESKIUC" --test-typechecker "$work/mf_ok.esk" "$work/mf_bad.esk" >/dev/null 2>&1 \
   && "$ESKIUC" "$work/mf_a.esk" "$work/mf_b.esk" "$work/mf_c.esk" -o "$work/mf" >/dev/null 2>&1 && "$work/mf"; then
    ok "cli/test-modes-all-inputs"
else
    bad "cli/test-modes-all-inputs" "a test mode ignored an extra input"
fi

# A lexical error in an imported file is reported once, with no parse errors after it.
lex_out="$("$ESKIUC" --test-typechecker "$here/errors/import_lex_error.esk" 2>&1)"
if [[ "$(printf '%s\n' "$lex_out" | grep -c '^error:')" -eq 1 ]]; then
    ok "cli/import-lex-error-stops"
else
    bad "cli/import-lex-error-stops" "$(printf '%s' "$lex_out" | grep '^error:' | tail -1)"
fi

# A NUL byte in code is a located lexical error (one in a string literal is kept).
printf 'int main() {\n    string s = "a\000b";\n    \000 return 0;\n}\n' > "$work/nul.esk"
nul_out="$("$ESKIUC" --test-typechecker "$work/nul.esk" 2>&1)"
if [[ "$nul_out" == *"nul.esk:3:5: unexpected byte 0x00"* && "$(printf '%s\n' "$nul_out" | grep -c '^error:')" -eq 1 ]]; then
    ok "cli/nul-byte"
else
    bad "cli/nul-byte" "$(printf '%s' "$nul_out" | grep -m1 error)"
fi

# __FILE__ is a well-formed string literal even when the path has a quote or a backslash.
qdir="$work/q\"d\\ir"
mkdir -p "$qdir"
printf 'extern int printf(string fmt, ...);\nint main() { printf("%%s\\n", __FILE__); return 0; }\n' > "$qdir/f.esk"
if "$ESKIUC" "$qdir/f.esk" -o "$work/fq" >/dev/null 2>&1 && [[ "$("$work/fq")" == "$qdir/f.esk" ]]; then
    ok "cli/file-macro-escape"
else
    bad "cli/file-macro-escape" "__FILE__ for '$qdir/f.esk' is not the path"
fi

# --freestanding at -O2 keeps byte loops as loops: no memset/memcpy call that no libc
# provides. The same loops in a hosted build may become those calls.
fs_obj="$work/fs_nb.o"
if "$ESKIUC" "$here/run_cmd/freestanding_no_builtins.esk" --freestanding --target aarch64-unknown-none-elf \
       -O2 -c -o "$fs_obj" >/dev/null 2>&1 && [[ -s "$fs_obj" ]] && ! grep -aqE 'memset|memcpy|memmove|malloc' "$fs_obj"; then
    ok "codegen/freestanding-no-builtins"
else
    bad "codegen/freestanding-no-builtins" "a freestanding -O2 object references memset/memcpy"
fi

# Narrow C integers: like clang, AArch64 outside Darwin and Windows x64 put no signext or
# zeroext on extern params and results (bool stays zeroext on Windows x64); Darwin does.
na_l=$("$ESKIUC" "$here/c_abi_narrow.esk" --target aarch64-unknown-linux-gnu --test-codegen 2>/dev/null | grep -cE '^declare .*(signext|zeroext)')
na_w=$("$ESKIUC" "$here/c_abi_narrow.esk" --target x86_64-w64-windows-gnu --test-codegen 2>/dev/null | grep -E '^declare ' | grep -E '(signext|zeroext)' | grep -cv 'i1 zeroext')
na_d=$("$ESKIUC" "$here/c_abi_narrow.esk" --target arm64-apple-macosx --test-codegen 2>/dev/null | grep -cE '^declare .*signext')
if [[ "$na_l" == 0 && "$na_w" == 0 && "$na_d" -gt 0 ]]; then ok "codegen/narrow-int-ext-per-target"
else bad "codegen/narrow-int-ext-per-target" "aarch64-linux=$na_l win64=$na_w darwin=$na_d"; fi

# C math externs that touch no memory are memory(none): floor everywhere, sqrt and powf
# only on Darwin (clang's -fno-math-errno default there); an int-typed cbrt never.
libm_pure() { "$ESKIUC" "$here/run_cmd/libm_pure.esk" --target "$1" --test-codegen 2>/dev/null | awk '/^attributes #[0-9]+ = .*memory\(none\)/ { p[$2] = 1 } { l[NR] = $0 } END { c = 0; for (i = 1; i <= NR; i++) if (l[i] ~ /^declare .*@(sqrt|floor|powf|cbrt)\(/) { if (l[i] ~ /memory\(none\)/) c++; else if (match(l[i], /#[0-9]+/) && p[substr(l[i], RSTART, RLENGTH)]) c++ } print c }'; }
lp_d=$(libm_pure arm64-apple-macosx); lp_l=$(libm_pure aarch64-unknown-linux-gnu)
if [[ "$lp_d" == 3 && "$lp_l" == 1 ]]; then ok "codegen/libm-pure-per-target"
else bad "codegen/libm-pure-per-target" "darwin=$lp_d (want 3) linux=$lp_l (want 1)"; fi

# Fields of packed structs are accessed at the alignment their address is known to have:
# 14 accesses in the test's functions are below their type's alignment (1, 2 or 4).
pfa=$("$ESKIUC" "$here/packed_field_align.esk" --test-codegen 2>/dev/null | grep -E '(= load|store) (volatile )?(i16|i32|i64|double|float)[ ,]' | awk '{ t = ""; for (i = 1; i <= NF; i++) if ($i == "load" || $i == "store") { t = $(i + 1); if (t == "volatile") t = $(i + 2); break } sub(/,/, "", t); nat = (t == "i16") ? 2 : ((t == "i32" || t == "float") ? 4 : 8); al = nat; if (match($0, /align [0-9]+$/)) al = substr($0, RSTART + 6) + 0; if (al < nat) n++ } END { print n + 0 }')
if [[ "$pfa" == 14 ]]; then ok "codegen/packed-field-align"
else bad "codegen/packed-field-align" "$pfa accesses below natural alignment (want 14)"; fi

# An output named .obj is an object file (Windows spelling), not an executable to link.
printf 'int main() { return 0; }\n' > "$work/obj.esk"
if "$ESKIUC" "$work/obj.esk" -o "$work/obj.obj" >/dev/null 2>&1 && [[ -s "$work/obj.obj" ]] \
   && ! "$work/obj.obj" >/dev/null 2>&1; then
    ok "cli/obj-output"
else
    bad "cli/obj-output" "-o x.obj did not write an object file"
fi

# The native macOS triple carries the macOS product version, so the linker does not
# warn that the object targets a newer macOS than the one it links for.
if [[ "$(uname -s)" == Darwin ]]; then
    mac_out=$("$ESKIUC" "$work/obj.esk" -o "$work/objexe" 2>&1)
    if [[ "$mac_out" != *"built for newer"* ]]; then ok "cli/macos-version"
    else bad "cli/macos-version" "$mac_out"; fi
fi

# `--help` documents the subcommands and lists only Eskiu's options (the LLVM
# backend's internal flags are hidden).
help_out="$("$ESKIUC" --help 2>&1)"
if [[ "$help_out" == *"eskiuc run"* && "$help_out" == *"eskiuc fmt"* && "$help_out" != *"aarch64-neon-syntax"* ]]; then
    ok "cli/help"
else
    bad "cli/help" "subcommands missing or LLVM options listed"
fi

# The editor's standalone LSP server (editor/vscode/server.js), when node exists.
if command -v node >/dev/null 2>&1; then
    lsp_out="$(node "$here/editor/lsp_server_test.js" 2>&1)"
    if [[ "$lsp_out" == "ok" ]]; then ok "editor/lsp-server"; else bad "editor/lsp-server" "$lsp_out"; fi
fi

# ---- go-to-definition and hover ---------------------------------------------
# tests/lsp/NAME.esk lists `// DEF L:C L2:C2` queries: --definition-at L:C must
# resolve to L2:C2 in the same file (the symbol scope lookup picked, not a
# same-named one elsewhere).
# `// HOVER L:C TYPE` queries: --hover-at L:C must resolve to TYPE.
echo "Go-to-definition and hover:"
for esk in "$here"/lsp/*.esk; do
    [[ -e "$esk" ]] || continue
    name="lsp/$(basename "$esk" .esk)"
    while read -r q want; do
        got="$("$ESKIUC" "$esk" --definition-at "$q" 2>&1)"
        if [[ "$got" == "$esk:$want" ]]; then
            ok "$name $q"
        else
            bad "$name $q" "expected $want, got '$got'"
        fi
    done < <(grep '^// DEF ' "$esk" | sed 's#^// DEF ##')

    while read -r q want; do
        got="$("$ESKIUC" "$esk" --hover-at "$q" 2>/dev/null | tail -n 1)"
        if [[ "$got" == "$want" ]]; then
            ok "$name hover $q"
        else
            bad "$name hover $q" "expected '$want', got '$got'"
        fi
    done < <(grep '^// HOVER ' "$esk" | sed 's#^// HOVER ##')

    # `// REFS L:C L1:C1 L2:C2 ...`: --references-at L:C lists exactly those places.
    while read -r q want; do
        got="$("$ESKIUC" "$esk" --references-at "$q" 2>&1 | sed 's#^.*:\([0-9]*:[0-9]*\):[0-9]*$#\1#' | tr '\n' ' ' | sed 's/ $//')"
        if [[ "$got" == "$want" ]]; then
            ok "$name refs $q"
        else
            bad "$name refs $q" "expected '$want', got '$got'"
        fi
    done < <(grep '^// REFS ' "$esk" | sed 's#^// REFS ##')

    # `// COMPLETE L:C NAME...`: --complete-at L:C must offer every NAME.
    while read -r q names; do
        got="$("$ESKIUC" "$esk" --complete-at "$q" 2>/dev/null | cut -f2)"
        missing=""
        for n in $names; do grep -qx -- "$n" <<<"$got" || missing="$missing $n"; done
        if [[ -z "$missing" ]]; then
            ok "$name complete $q"
        else
            bad "$name complete $q" "missing:$missing"
        fi
    done < <(grep '^// COMPLETE ' "$esk" | sed 's#^// COMPLETE ##')
done

# --symbols lists the file's declarations (an editor outline), fields and inline
# methods under their struct.
got_syms="$("$ESKIUC" "$here/lsp/members.esk" --symbols 2>/dev/null | cut -f1-4 | tr '\t' ' ')"
want_syms="struct Point 23:8 
field x 24:9 Point
field y 25:9 Point
method sum 26:9 Point
function Point_dist 29:5 
function add 31:5 
function main 33:5 "
if [[ "$got_syms" == "$want_syms" ]]; then
    ok "lsp/symbols"
else
    bad "lsp/symbols" "got '$got_syms'"
fi

# LSP error handling: parse error, semantic error, and missing symbol
echo "LSP diagnostics:"
lsp_tmp="$work/lsp_syntax_err.tmp.esk"
echo "int f( {" > "$lsp_tmp"
got_parse="$("$ESKIUC" "$lsp_tmp" --hover-at 1:1 2>/dev/null | tail -n 1)"
if [[ "$got_parse" == "(parse error)" ]]; then
    ok "lsp/hover parse error"
else
    bad "lsp/hover parse error" "expected '(parse error)', got '$got_parse'"
fi

got_parse_def="$("$ESKIUC" "$lsp_tmp" --definition-at 1:1 2>/dev/null | tail -n 1)"
if [[ "$got_parse_def" == "(parse error)" ]]; then
    ok "lsp/definition parse error"
else
    bad "lsp/definition parse error" "expected '(parse error)', got '$got_parse_def'"
fi

lsp_sema_tmp="$work/lsp_sema_err.tmp.esk"
echo "int f() { return undefined_var; }" > "$lsp_sema_tmp"
got_sema="$("$ESKIUC" "$lsp_sema_tmp" --hover-at 1:18 2>/dev/null | tail -n 1)"
if [[ "$got_sema" == "(semantic error)" ]]; then
    ok "lsp/hover semantic error"
else
    bad "lsp/hover semantic error" "expected '(semantic error)', got '$got_sema'"
fi

got_sema_def="$("$ESKIUC" "$lsp_sema_tmp" --definition-at 1:18 2>/dev/null | tail -n 1)"
if [[ "$got_sema_def" == "(semantic error)" ]]; then
    ok "lsp/definition semantic error"
else
    bad "lsp/definition semantic error" "expected '(semantic error)', got '$got_sema_def'"
fi

got_empty_hover="$("$ESKIUC" "$here/lsp/scoped_defs.esk" --hover-at 1:1 2>/dev/null | tail -n 1)"
if [[ "$got_empty_hover" == "(no type at 1:1)" ]]; then
    ok "lsp/hover no symbol"
else
    bad "lsp/hover no symbol" "expected '(no type at 1:1)', got '$got_empty_hover'"
fi

got_empty_def="$("$ESKIUC" "$here/lsp/scoped_defs.esk" --definition-at 1:1 2>/dev/null | tail -n 1)"
if [[ "$got_empty_def" == "(no definition at 1:1)" ]]; then
    ok "lsp/definition no symbol"
else
    bad "lsp/definition no symbol" "expected '(no definition at 1:1)', got '$got_empty_def'"
fi

# ---- formatter idempotency ------------------------------------------------
# `eskiuc fmt` must be idempotent: formatting an already-formatted file is a
# no-op. Format every positive test into a temp file, then assert `fmt --check`
# reports no further change.
echo "Formatter idempotency:"
for esk in "$here"/*.esk; do
    name="$(basename "$esk" .esk)"
    cp "$esk" "$work/$name.fmt.esk"
    "$ESKIUC" fmt "$work/$name.fmt.esk" >/dev/null 2>&1
    if "$ESKIUC" fmt --check "$work/$name.fmt.esk" >/dev/null 2>&1; then
        ok "fmt/$name"
    else
        bad "fmt/$name" "fmt is not idempotent"
    fi
done

# `eskiuc fmt` output: tests/fmt_cases/NAME.esk must format to NAME.formatted
# byte for byte (line endings included).
echo "Formatter output:"
for esk in "$here"/fmt_cases/*.esk; do
    [[ -e "$esk" ]] || continue
    name="fmt_cases/$(basename "$esk" .esk)"
    cp "$esk" "$work/fmtcase.esk"
    "$ESKIUC" fmt "$work/fmtcase.esk" >/dev/null 2>&1
    if cmp -s "$work/fmtcase.esk" "${esk%.esk}.formatted"; then ok "$name"
    else bad "$name" "formatted output differs from ${name}.formatted"; fi
done

# `eskiuc fmt` never changes behavior: a fmt case that builds must build and run the
# same (exit code + stdout) after formatting.
echo "Formatter behavior:"
for esk in "$here"/fmt_cases/*.esk; do
    [[ -e "$esk" ]] || continue
    name="fmt_cases/$(basename "$esk" .esk)"
    "$ESKIUC" "$esk" -o "$work/fmtb.orig" >/dev/null 2>&1 || continue
    cp "$esk" "$work/fmtb.esk"
    "$ESKIUC" fmt "$work/fmtb.esk" >/dev/null 2>&1
    if ! "$ESKIUC" "$work/fmtb.esk" -o "$work/fmtb.fmt" >/dev/null 2>&1; then
        bad "$name/behavior" "the formatted file no longer builds"; continue
    fi
    o1="$("$work/fmtb.orig" 2>&1)"; c1=$?
    o2="$("$work/fmtb.fmt" 2>&1)"; c2=$?
    if [[ "$c1" == "$c2" && "$o1" == "$o2" ]]; then ok "$name/behavior"
    else bad "$name/behavior" "formatting changed the program (exit $c1 -> $c2)"; fi
done

# ---- C++ unit tests -------------------------------------------------------
# The typed `Type` IR (sema/type.{h,cpp}) is a standalone, dependency-free unit;
# its round-trip invariant (parse(s).str()==s) is checked here.
echo "Unit tests:"
rt_src="$here/type_roundtrip/roundtrip_test.cpp"
if [ -f "$rt_src" ]; then
    if "${CXX:-clang++}" -std=c++17 "$rt_src" "$root/sema/type.cpp" -I"$root/sema" \
            -o "$work/type_roundtrip" 2>"$work/rt_cerr"; then
        if "$work/type_roundtrip" >/dev/null 2>&1; then ok "unit/type_roundtrip"
        else bad "unit/type_roundtrip" "round-trip assertions failed"; fi
    else
        bad "unit/type_roundtrip" "compile failed: $(head -1 "$work/rt_cerr")"
    fi
fi

# ---- structural LLVM IR assertions ----------------------------------------
if [ -x "$here/ir_golden.sh" ]; then
    echo "Structural LLVM IR checks:"
    if "$here/ir_golden.sh" "$ESKIUC" >"$work/ir_golden.log" 2>&1; then
        ok "ir/golden_assertions"
    else
        bad "ir/golden_assertions" "$(cat "$work/ir_golden.log")"
    fi
fi

# ---- test catalog ---------------------------------------------------------
# Every test describes itself in its first comment (tests/index.sh builds the catalog).
echo "Test catalog:"
if "$here/index.sh" --check >"$work/index.log" 2>&1; then
    ok "index/descriptions"
else
    bad "index/descriptions" "$(cat "$work/index.log")"
fi

# ---- summary --------------------------------------------------------------
echo
echo "------------------------------------------------------------"
printf 'Results: \033[32m%d passed\033[0m, \033[31m%d failed\033[0m\n' "$pass" "$fail"
if [[ $fail -gt 0 ]]; then
    printf 'Failures: %s\n' "${failed_names[*]}"
    exit 1
fi
exit 0
