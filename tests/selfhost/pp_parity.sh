#!/usr/bin/env bash
# Parity gate for the self-hosted preprocessor (selfhost/preprocessor.esk) vs the
# C++ preprocessor (lexer/preprocessor.cpp).
#
# There is no standalone --test-preprocess oracle, and there needn't be: the C++
# `--test-lexer` builds a Lexer, which runs preprocess() before tokenizing. So we
# validate the preprocessor THROUGH the lexer — `pp_main` preprocesses + lexes and
# prints token lines in the exact `--test-lexer` format; we diff against the C++
# `--test-lexer`. Byte-identical token streams = the two preprocessors agree (any
# __LINE__/expansion/conditional difference surfaces as a different token).
#
# `--test-lexer` constructs `Lexer(source)` with an EMPTY filename and no predefined
# OS macros, so pp_main preprocesses with filename "" to match (__FILE__ -> "").
#
# Usage: tests/selfhost/pp_parity.sh [file.esk ...]
#   no args   -> the synthetic corpus under tests/selfhost/pp_inputs/
#   --full    -> the whole real corpus: tests/*.esk + stdlib/*.esk. No exclusions —
#                the self-hosted preprocessor handles every directive, so clean and
#                directive-using files alike must match.
# Green (exit 0) = byte-identical token streams for every file.

set -u
cd "$(dirname "$0")/../.." || exit 2

BIN="${ESKIUC:-}"
if [ -z "$BIN" ]; then
    if [ -x build/eskiuc ]; then BIN=build/eskiuc
    elif command -v eskiuc >/dev/null 2>&1; then BIN=eskiuc
    else echo "pp_parity: cannot find eskiuc (set ESKIUC or build it)"; exit 2; fi
fi

DRIVER=selfhost/pp_main.esk

# Compile the Eskiu driver ONCE to a native binary and run that per file.
PPBIN="$(mktemp -t pp_main.XXXXXX)"
trap 'rm -f "$PPBIN"' EXIT
if ! "$BIN" "$DRIVER" -o "$PPBIN" >/dev/null 2>/tmp/ppbuild.$$; then
    echo "pp_parity: failed to build $DRIVER"; cat /tmp/ppbuild.$$; rm -f /tmp/ppbuild.$$; exit 2
fi
rm -f /tmp/ppbuild.$$

strip_banner() {
    LC_ALL=C sed -E '/^Tokenizing: /d; /^=+$/d; /^Total tokens: /d'
}

if [ "$#" -eq 1 ] && [ "$1" = "--full" ]; then
    files=(tests/*.esk stdlib/*.esk stdlib/c/*.esk)
    echo "corpus: ${#files[@]} files (tests/ + stdlib/, no exclusions)"
    echo "----"
elif [ "$#" -gt 0 ]; then
    files=("$@")
else
    files=(tests/selfhost/pp_inputs/*.esk)
fi

fail=0
total=0
for f in "${files[@]}"; do
    [ -f "$f" ] || { echo "MISS  $f (no such file)"; fail=1; continue; }
    total=$((total + 1))
    ref=$("$BIN" --test-lexer "$f" 2>/dev/null | strip_banner)
    got=$("$PPBIN" "$f" 2>/dev/null)
    if [ "$ref" = "$got" ]; then
        echo "ok    $f"
    else
        echo "FAIL  $f"
        diff <(printf '%s\n' "$ref") <(printf '%s\n' "$got") | sed 's/^/      /'
        fail=1
    fi
done

# __FILE__ for a path with a quote and a backslash (escaped into one string literal).
QDIR="$(mktemp -d)"
mkdir -p "$QDIR/q\"d\\ir"
printf 'string f = __FILE__;\n' > "$QDIR/q\"d\\ir/f.esk"
total=$((total + 1))
ref=$("$BIN" --test-lexer "$QDIR/q\"d\\ir/f.esk" 2>/dev/null | strip_banner)
got=$("$PPBIN" "$QDIR/q\"d\\ir/f.esk" 2>/dev/null)
if [ "$ref" = "$got" ] && printf '%s' "$ref" | grep -qF "q\"d\\ir/f.esk'"; then
    echo "ok    __FILE__ escaping"
else
    echo "FAIL  __FILE__ escaping"
    diff <(printf '%s\n' "$ref") <(printf '%s\n' "$got") | sed 's/^/      /'
    fail=1
fi
rm -rf "$QDIR"

echo "----"
if [ "$fail" -eq 0 ]; then
    echo "parity: $total/$total files match"
else
    echo "parity: MISMATCH"
fi
exit "$fail"
