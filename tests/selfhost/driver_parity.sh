#!/usr/bin/env bash
# Driver parity gate for the self-hosted compiler driver (selfhost/esk_main.esk).
#
# Unlike cg_parity (which compares raw codegen), this exercises the WHOLE driver:
# `esk_main <file.esk> -o <out>` parses, type-checks, codegens, writes a temp .ll, and
# invokes clang to link a native binary — the same end-to-end path the C++ `eskiuc`
# takes. Oracle is behavioral: run both binaries and compare exit code + stdout.
#
# Usage: tests/selfhost/driver_parity.sh [file.esk ...]
#   no args -> the corpus under tests/selfhost/driver_inputs/
# Green (exit 0) = identical exit code and stdout for every program.

set -u
cd "$(dirname "$0")/../.." || exit 2
ROOT="$(pwd)"

BIN="${ESKIUC:-}"
if [ -z "$BIN" ]; then
    if [ -x build/eskiuc ]; then BIN=build/eskiuc
    elif command -v eskiuc >/dev/null 2>&1; then BIN=eskiuc
    else echo "driver_parity: cannot find eskiuc (set ESKIUC or build it)"; exit 2; fi
fi
CLANG="${CLANG:-clang}"   # the driver shells out to $CLANG (CI installs it as clang-22)
export CLANG
command -v "$CLANG" >/dev/null 2>&1 || { echo "driver_parity: $CLANG not found (set CLANG)"; exit 2; }

# Build the self-hosted driver with the C++ compiler.
ESKMAIN="$(mktemp -t esk_main.XXXXXX)"
WORK="$(mktemp -d)"
trap 'rm -f "$ESKMAIN"; rm -rf "$WORK"' EXIT
if ! "$BIN" selfhost/esk_main.esk -o "$ESKMAIN" >/dev/null 2>"$WORK/build.log"; then
    echo "driver_parity: failed to build selfhost/esk_main.esk"; cat "$WORK/build.log"; exit 2
fi

if [ "$#" -gt 0 ]; then files=("$@"); else files=(tests/selfhost/driver_inputs/*.esk); fi

fail=0
total=0
# Build FILE with both drivers, run both binaries, compare exit code + stdout.
check_prog() {
    local f="$1" base self_out self_code cpp_out cpp_code
    [ -f "$f" ] || { echo "MISS  $f"; fail=1; return; }
    total=$((total + 1))
    base="$(basename "$f" .esk)"

    # Self-hosted driver: parse → sema → codegen → clang, all in the Eskiu binary.
    if ! ESKIU_ROOT="$ROOT" "$ESKMAIN" "$f" -o "$WORK/$base.self" 2>"$WORK/$base.self.err"; then
        echo "FAIL  $base  (self-host driver errored)"; sed 's/^/      /' "$WORK/$base.self.err" | grep -v 'overriding the module' | head; fail=1; return
    fi
    self_out="$("$WORK/$base.self" 2>/dev/null)"; self_code=$?

    # Reference: the C++ driver.
    if ! "$BIN" "$f" -o "$WORK/$base.cpp" >/dev/null 2>&1; then
        echo "skip  $base  (C++ eskiuc could not build it)"; total=$((total - 1)); return
    fi
    cpp_out="$("$WORK/$base.cpp" 2>/dev/null)"; cpp_code=$?

    if [ "$self_code" = "$cpp_code" ] && [ "$self_out" = "$cpp_out" ]; then
        echo "ok    $base  (exit $self_code)"
    else
        echo "FAIL  $base  (self exit=$self_code out=$self_out | cpp exit=$cpp_code out=$cpp_out)"; fail=1
    fi
}
for f in "${files[@]}"; do check_prog "$f"; done

# Deep input (tests/deep/gen.sh): long operator chains and deep nesting compile to the
# same program in both drivers, and nesting past the parser's limit is the same clean
# error (exit 1, not a crash) in both.
if bash tests/deep/gen.sh "$WORK/deep"; then
    for f in "$WORK"/deep/*.esk; do
        want="$(head -1 "$f" | grep 'EXPECT-ERROR:' | sed 's/.*EXPECT-ERROR:[[:space:]]*//')"
        if [ -z "$want" ]; then check_prog "$f"; continue; fi
        total=$((total + 1))
        base="deep_$(basename "$f" .esk)"
        c=0; "$BIN" "$f" -o "$WORK/$base.cpp" >"$WORK/$base.cpp.err" 2>&1 || c=$?
        s=0; ESKIU_ROOT="$ROOT" "$ESKMAIN" "$f" -o "$WORK/$base.self" >"$WORK/$base.self.err" 2>&1 || s=$?
        if [ "$c" = 1 ] && [ "$s" = 1 ] && grep -qF "$want" "$WORK/$base.cpp.err" && grep -qF "$want" "$WORK/$base.self.err"; then
            echo "ok    $base  (rejected: $want)"
        else
            echo "FAIL  $base  (cpp exit=$c, self exit=$s; want exit 1 with \"$want\")"; fail=1
        fi
    done
else
    echo "FAIL  deep/gen"; fail=1
fi

# Flags: both drivers accept the same compile flags and reject unknown ones.
ARGS_ESK=tests/run_cmd/args.esk
flagcheck() { # name expect(ok|reject) flags...
    local name="$1" want="$2"; shift 2
    total=$((total + 1))
    local c=0 s=0
    ESKIU_ROOT="$ROOT" "$BIN" "$@" >/dev/null 2>&1 || c=1
    ESKIU_ROOT="$ROOT" "$ESKMAIN" "$@" >/dev/null 2>&1 || s=1
    local w=0; [ "$want" = reject ] && w=1
    if [ "$c" = "$w" ] && [ "$s" = "$w" ]; then echo "ok    flags/$name"
    else echo "FAIL  flags/$name  (cpp rc!=0: $c, self rc!=0: $s, want $want)"; fail=1; fi
}
flagcheck object   ok     -c "$ARGS_ESK" -o "$WORK/f.o"
flagcheck opt      ok     -O2 "$ARGS_ESK" -o "$WORK/f.o2"
flagcheck optsep   ok     -O 2 "$ARGS_ESK" -o "$WORK/f.o2s"
flagcheck badoptsep reject -O 9 "$ARGS_ESK" -o "$WORK/f.o9s"
flagcheck unknown  reject --no-such-flag "$ARGS_ESK" -o "$WORK/f.u"
flagcheck linklib  ok     -lm "$ARGS_ESK" -o "$WORK/f.lm"
flagcheck badopt   reject -O9 "$ARGS_ESK" -o "$WORK/f.o9"
flagcheck dirinput reject "$WORK" -o "$WORK/f.dir"
# Exit codes (docs/lang/build.md): a wrong command line is 2 in both drivers, and
# `fmt --help` prints the same usage with exit 0; `fmt` with a wrong option says the same.
usagecheck() { # name want-rc args...
    local name="$1" want="$2"; shift 2
    total=$((total + 1))
    local c s co so
    co="$(ESKIU_ROOT="$ROOT" "$BIN" "$@" 2>&1)"; c=$?
    so="$(ESKIU_ROOT="$ROOT" "$ESKMAIN" "$@" 2>&1)"; s=$?
    if [ "$c" = "$want" ] && [ "$s" = "$want" ] && { [ "$want" = 2 ] && [ "${name#fmt}" = "$name" ] || [ "$co" = "$so" ]; }; then
        echo "ok    usage/$name"
    else echo "FAIL  usage/$name  (cpp rc $c, self rc $s, want $want; cpp: $co | self: $so)"; fail=1; fi
}
usagecheck unknown   2 --no-such-flag "$ARGS_ESK" -o "$WORK/f.u"
usagecheck badopt    2 -O9 "$ARGS_ESK" -o "$WORK/f.o9"
usagecheck noinput   2
usagecheck fmt-help  0 fmt --help
usagecheck fmt-h     0 fmt -h
usagecheck fmt-opt   2 fmt --no-such-flag
usagecheck fmt-width 2 fmt --width
cp "$ARGS_ESK" "$WORK/ow.esk"
flagcheck overwrite reject "$WORK/ow.esk" -o "$WORK/ow.esk"
total=$((total + 1))
if cmp -s "$ARGS_ESK" "$WORK/ow.esk"; then echo "ok    flags/input-kept"
else echo "FAIL  flags/input-kept  (-o over the input replaced it)"; fail=1; fi
total=$((total + 1))
if [ -s "$WORK/f.o" ] && ! [ -x "$WORK/f.o" ]; then echo "ok    flags/c-writes-object"
else echo "FAIL  flags/c-writes-object"; fail=1; fi
# No -o: both drivers write the object <input>.o and print nothing on stdout.
for who in cpp self; do
    total=$((total + 1))
    mkdir -p "$WORK/noo_$who"; cp "$ARGS_ESK" "$WORK/noo_$who/a.esk"
    if [ "$who" = cpp ]; then out="$("$BIN" "$WORK/noo_$who/a.esk" 2>/dev/null)"; rc=$?
    else out="$(ESKIU_ROOT="$ROOT" "$ESKMAIN" "$WORK/noo_$who/a.esk" 2>/dev/null)"; rc=$?; fi
    if [ "$rc" = 0 ] && [ -z "$out" ] && [ -s "$WORK/noo_$who/a.esk.o" ] && ! [ -x "$WORK/noo_$who/a.esk.o" ]; then
        echo "ok    flags/no-o-object ($who)"
    else echo "FAIL  flags/no-o-object ($who)  (rc=$rc, stdout bytes=${#out})"; fail=1; fi
done

# Multi-file: an `extern` variable in one input names the definition in another (either
# input order), in both drivers.
for order in "main lib" "lib main"; do
    read -r mfa mfb <<< "$order"
    total=$((total + 1))
    mfin=("tests/multi_extern/$mfa.esk" "tests/multi_extern/$mfb.esk")
    c=0; s=0
    "$BIN" "${mfin[@]}" -o "$WORK/mf.cpp" >/dev/null 2>&1 && "$WORK/mf.cpp" || c=$?
    ESKIU_ROOT="$ROOT" "$ESKMAIN" "${mfin[@]}" -o "$WORK/mf.self" >/dev/null 2>&1 && "$WORK/mf.self" || s=$?
    if [ "$c" = 42 ] && [ "$s" = 42 ]; then echo "ok    multi-file/extern ($mfa $mfb)"
    else echo "FAIL  multi-file/extern ($mfa $mfb)  (cpp exit=$c, self exit=$s, want 42)"; fail=1; fi
done

# Separate compilation: a prototype defined in another object file is declared, so the
# object each driver writes for main.esk links with lib.esk's.
total=$((total + 1))
c=0; s=0
"$BIN" -c tests/separate/lib.esk -o "$WORK/sep_lib.o" >/dev/null 2>&1 || c=1
"$BIN" -c tests/separate/main.esk -o "$WORK/sep_cpp.o" >/dev/null 2>&1 || c=1
ESKIU_ROOT="$ROOT" "$ESKMAIN" -c tests/separate/main.esk -o "$WORK/sep_self.o" >/dev/null 2>&1 || s=1
"$CLANG" "$WORK/sep_cpp.o" "$WORK/sep_lib.o" -o "$WORK/sep_cpp" >/dev/null 2>&1 && "$WORK/sep_cpp" || c=$?
"$CLANG" "$WORK/sep_self.o" "$WORK/sep_lib.o" -o "$WORK/sep_self" >/dev/null 2>&1 && "$WORK/sep_self" || s=$?
if [ "$c" = 42 ] && [ "$s" = 42 ]; then echo "ok    separate/prototype"
else echo "FAIL  separate/prototype  (cpp exit=$c, self exit=$s, want 42)"; fail=1; fi

# Implied libraries: both drivers must pass the linker the same -l flags (#pragma link,
# the C++ exception runtime, pthread, per target; explicit -l not repeated; none under
# --no-default-libs). A wrapper standing in for $CC / $CLANG records them; a cross
# target's link is expected to fail, only the recorded flags matter.
REALCC="$(command -v "$CLANG")"
cat > "$WORK/cclog.sh" <<EOF
#!/bin/sh
for a in "\$@"; do case "\$a" in -l*) printf '%s ' "\$a" >> "\$CCLOG" ;; esac; done
exec "$REALCC" "\$@"
EOF
chmod +x "$WORK/cclog.sh"
libcheck() { # name flags...
    local name="$1"; shift
    total=$((total + 1))
    rm -f "$WORK/l.cpp" "$WORK/l.self"; touch "$WORK/l.cpp" "$WORK/l.self"
    CCLOG="$WORK/l.cpp" CC="$WORK/cclog.sh" ESKIU_ROOT="$ROOT" "$BIN" "$@" -o "$WORK/l.bin" >/dev/null 2>&1
    CCLOG="$WORK/l.self" CLANG="$WORK/cclog.sh" ESKIU_ROOT="$ROOT" "$ESKMAIN" "$@" -o "$WORK/l.bin" >/dev/null 2>&1
    if cmp -s "$WORK/l.cpp" "$WORK/l.self"; then echo "ok    libs/$name  ($(cat "$WORK/l.cpp"))"
    else echo "FAIL  libs/$name  (cpp: $(cat "$WORK/l.cpp")| self: $(cat "$WORK/l.self"))"; fail=1; fi
}
for tgt in "" x86_64-unknown-linux-gnu x86_64-w64-windows-gnu aarch64-none-elf; do
    tf=(); [ -n "$tgt" ] && tf=(--target "$tgt")
    tn="${tgt:-host}"
    libcheck "pragma-$tn"     ${tf[@]+"${tf[@]}"} tests/pragma_link.esk
    libcheck "math-$tn"       ${tf[@]+"${tf[@]}"} tests/math_nolib.esk -lm
    libcheck "exceptions-$tn" ${tf[@]+"${tf[@]}"} tests/exceptions.esk
    libcheck "threads-$tn"    ${tf[@]+"${tf[@]}"} tests/threads.esk
    libcheck "net-$tn"        ${tf[@]+"${tf[@]}"} tests/net_echo.esk
done
# Diagnostics: a syntax error is reported on stderr as `error: file:line:col: msg`,
# identically by both drivers (diag_parse/), and a type error is reported on stderr with
# nothing on stdout (diag_sema/). Both drivers must reject every file.
for f in tests/selfhost/driver_inputs/diag_parse/*.esk tests/selfhost/driver_inputs/diag_sema/*.esk; do
    total=$((total + 1))
    n="diag/$(basename "$(dirname "$f")")/$(basename "$f" .esk)"
    "$BIN" "$f" -o "$WORK/d.bin" >"$WORK/d.cpp.out" 2>"$WORK/d.cpp.err"; cc=$?
    ESKIU_ROOT="$ROOT" "$ESKMAIN" "$f" -o "$WORK/d.bin" >"$WORK/d.self.out" 2>"$WORK/d.self.err"; sc=$?
    ce="$(grep -m1 '^error: ' "$WORK/d.cpp.err")"; se="$(grep -m1 '^error: ' "$WORK/d.self.err")"
    ok=1
    [ "$cc" -ne 0 ] && [ "$sc" -ne 0 ] || ok=0
    [ -s "$WORK/d.self.out" ] && ok=0
    case "$se" in "error: $f:"*) ;; *) ok=0 ;; esac
    case "$f" in */diag_parse/*) [ "$ce" = "$se" ] || ok=0 ;; esac
    if [ "$ok" -eq 1 ]; then echo "ok    $n"
    else echo "FAIL  $n  (cpp rc $cc: $ce | self rc $sc: $se | self stdout: $(head -c 200 "$WORK/d.self.out"))"; fail=1; fi
done

# --test-codegen type-checks first: an ill-typed program is rejected (exit 1, no IR).
total=$((total + 1))
ESKIU_ROOT="$ROOT" "$ESKMAIN" tests/selfhost/driver_inputs/diag_sema/bad_init.esk --test-codegen >"$WORK/tc.out" 2>/dev/null; sc=$?
if [ "$sc" -eq 1 ] && ! [ -s "$WORK/tc.out" ]; then echo "ok    flags/test-codegen-typechecks"
else echo "FAIL  flags/test-codegen-typechecks  (rc $sc, $(wc -c < "$WORK/tc.out") bytes of IR)"; fail=1; fi

# `run -- script.esk`: a `--` before the script ends the compiler flags.
total=$((total + 1))
ESKIU_ROOT="$ROOT" "$BIN" run -- tests/selfhost/driver_inputs/arith.esk >"$WORK/r.cpp" 2>/dev/null; cc=$?
ESKIU_ROOT="$ROOT" "$ESKMAIN" run -- tests/selfhost/driver_inputs/arith.esk >"$WORK/r.self" 2>/dev/null; sc=$?
if [ "$cc" = "$sc" ] && cmp -s "$WORK/r.cpp" "$WORK/r.self"; then echo "ok    flags/run-dashdash  (exit $sc)"
else echo "FAIL  flags/run-dashdash  (cpp exit $cc, self exit $sc)"; fail=1; fi

# The temporary IR never lands next to the output: a user's `<out>.ll` survives.
total=$((total + 1))
echo "user file" > "$WORK/keep.ll"
ESKIU_ROOT="$ROOT" "$ESKMAIN" tests/selfhost/driver_inputs/hello.esk -o "$WORK/keep" >/dev/null 2>&1
if [ -x "$WORK/keep" ] && [ "$(cat "$WORK/keep.ll")" = "user file" ]; then echo "ok    flags/temp-ll-unique"
else echo "FAIL  flags/temp-ll-unique  (<out>.ll was replaced or deleted)"; fail=1; fi

# `run` builds its program in $TMPDIR (not a fixed /tmp) and leaves nothing behind there.
for drv in "$BIN" "$ESKMAIN"; do
    total=$((total + 1))
    rm -rf "$WORK/tmpd"; mkdir -p "$WORK/tmpd"
    out="$(TMPDIR="$WORK/tmpd/" ESKIU_ROOT="$ROOT" "$drv" run tests/selfhost/driver_inputs/run_tmp/argv0_in_tmpdir.esk 2>&1)"; rc=$?
    left="$(ls -A "$WORK/tmpd")"
    if [ "$rc" = 0 ] && [ "$out" = "in-tmpdir" ] && [ -z "$left" ]; then echo "ok    flags/run-tmpdir  ($(basename "$drv"))"
    else echo "FAIL  flags/run-tmpdir  ($(basename "$drv"): rc $rc, output '$out', left '$left')"; fail=1; fi
done

# Target macros: _WIN64 accompanies _WIN32 on every 64-bit Windows triple (x86_64,
# aarch64 and arm64 spellings), and both drivers predefine the same set.
for tgt in x86_64-pc-windows-msvc aarch64-pc-windows-msvc arm64-pc-windows-msvc i686-pc-windows-msvc; do
    total=$((total + 1))
    "$BIN" --target "$tgt" tests/target_macros.esk --test-parser 2>&1 | grep -o 'is_win[0-9]*' > "$WORK/m.cpp"
    ESKIU_ROOT="$ROOT" "$ESKMAIN" --target "$tgt" tests/target_macros.esk --test-parser 2>&1 | grep -o 'is_win[0-9]*' > "$WORK/m.self"
    want="is_win32"; case "$tgt" in i686*) ;; *) want="is_win32 is_win64" ;; esac
    got="$(tr '\n' ' ' < "$WORK/m.cpp" | sed 's/ $//')"
    if [ "$got" = "$want" ] && cmp -s "$WORK/m.cpp" "$WORK/m.self"; then echo "ok    macros/$tgt  ($got)"
    else echo "FAIL  macros/$tgt  (cpp: $got | self: $(tr '\n' ' ' < "$WORK/m.self"))"; fail=1; fi
done
libcheck "no-default-libs" --no-default-libs tests/exceptions.esk

# --freestanding -O2: neither driver may turn a byte loop into a memset/memcpy call.
total=$((total + 1))
fsok=1
for drv in "$BIN" "$ESKMAIN"; do
    rm -f "$WORK/fs.o"
    ESKIU_ROOT="$ROOT" "$drv" tests/run_cmd/freestanding_no_builtins.esk --freestanding \
        --target aarch64-unknown-none-elf -O2 -c -o "$WORK/fs.o" >/dev/null 2>&1 || fsok=0
    { [ -s "$WORK/fs.o" ] && ! grep -aqE 'memset|memcpy|memmove|malloc' "$WORK/fs.o"; } || fsok=0
done
if [ "$fsok" -eq 1 ]; then echo "ok    freestanding-no-builtins"
else echo "FAIL  freestanding-no-builtins  (an object references memset/memcpy)"; fail=1; fi

# --mattr reaches the backend in both drivers: without FP, a double multiply is a call.
total=$((total + 1))
maok=1
nm_bin="$(dirname "${CLANG:-clang}")/llvm-nm"
[ -x "$nm_bin" ] || nm_bin=nm
for drv in "$BIN" "$ESKMAIN"; do
    rm -f "$WORK/ma.o"
    if ! ESKIU_ROOT="$ROOT" "$drv" tests/run_cmd/mattr_softfloat.esk --freestanding --target aarch64-unknown-none-elf \
        --mattr=-fp-armv8,-neon -O2 -c -o "$WORK/ma.o" >"$WORK/ma.log" 2>&1; then
        maok=0; echo "      $drv failed:"; sed 's/^/        /' "$WORK/ma.log" | head -5
    elif ! "$nm_bin" "$WORK/ma.o" 2>/dev/null | grep -q '__muldf3'; then
        # The symbol table, not a grep through the bytes: grep -a missed it on the macOS runner.
        maok=0; echo "      $drv: no __muldf3 in the object"
        { file "$WORK/ma.o"; "$nm_bin" "$WORK/ma.o"; sed 's/^/log: /' "$WORK/ma.log"; } 2>&1 | head -12 | sed 's/^/        /'
    fi
done
if [ "$maok" -eq 1 ]; then echo "ok    mattr-softfloat"
else echo "FAIL  mattr-softfloat  (--mattr did not reach the backend)"; fail=1; fi

echo "----"
if [ "$fail" -eq 0 ]; then echo "driver parity: $total/$total programs match"; else echo "driver parity: MISMATCH"; fi
exit "$fail"
