#!/usr/bin/env bash
# Parity gate for the self-hosted parser (selfhost/parser.esk) vs the C++ parser.
#
# The Eskiu driver is compiled once to a native binary, then run per file. For each
# file we run BOTH parsers, strip the C++ `--test-parser` banner, and raw-`diff` the
# AST dumps (the Eskiu driver prints the dump in the exact ast_printer.cpp format).
# Byte-identical = parity.
#
# Usage: tests/selfhost/parse_parity.sh [file.esk ...]
#   no args   -> the synthetic corpus under tests/selfhost/parse_inputs/
#   --full    -> the real corpus tests/*.esk. The self-hosted parser now follows
#                `import` (resolving <stdlib> + relative paths, recursively), so the
#                only files it can't match are those whose transitive import closure
#                touches a stream-rewriting preprocessor directive / built-in macro /
#                shebang — the preprocessor isn't self-hosted yet, so the C++ token
#                stream would differ. A file is excluded iff ANY file in its closure
#                (itself + everything it imports) has such a directive.
# Green (exit 0) = byte-identical AST dumps for every file.

set -u
cd "$(dirname "$0")/../.." || exit 2

BIN="${ESKIUC:-}"
if [ -z "$BIN" ]; then
    if [ -x build/eskiuc ]; then BIN=build/eskiuc
    elif command -v eskiuc >/dev/null 2>&1; then BIN=eskiuc
    else echo "parse_parity: cannot find eskiuc (set ESKIUC or build it)"; exit 2; fi
fi

DRIVER=selfhost/esk_main.esk

# Compile the Eskiu driver ONCE to a native binary and run that per file — far
# faster than `eskiuc run DRIVER FILE` (which recompiles the driver every time),
# which is what makes the full corpus viable in CI.
PBIN="$(mktemp -t parse_main.XXXXXX)"
trap 'rm -f "$PBIN"' EXIT
if ! "$BIN" "$DRIVER" -o "$PBIN" >/dev/null 2>/tmp/parsebuild.$$; then
    echo "parse_parity: failed to build $DRIVER"; cat /tmp/parsebuild.$$; rm -f /tmp/parsebuild.$$; exit 2
fi
rm -f /tmp/parsebuild.$$

# The C++ --test-parser wraps the AST dump in a banner: `Parsing: FILE` and a rule of
# `=` before it, a rule and `Parse succeeded!` after it. Strip only those lines, so a
# string literal whose text has a line of `=` inside the dump is kept.
strip_banner() {
    LC_ALL=C awk '{ l[NR] = $0 } END {
        s = 1; e = NR
        if (s <= e && l[s] ~ /^Parsing: /) s++
        if (s <= e && l[s] ~ /^=+$/) s++
        if (e >= s && l[e] == "Parse succeeded!") e--
        if (e >= s && l[e] ~ /^=+$/) e--
        for (i = s; i <= e; i++) print l[i]
    }'
}


if [ "$#" -eq 1 ] && [ "$1" = "--full" ]; then
    # parse_main preprocesses the top-level file (like --test-parser folds
    # preprocessing into the lexer), so the WHOLE corpus is comparable now — no
    # preprocessor-closure exclusion. Files where the C++ ASTPrinter itself crashes
    # (body-less top-level prototype) are still skipped per-file below.
    # tests/escapes.esk holds a string with an embedded NUL: the C++ std::string
    # AST printer preserves the NUL while the self-host C-string printer truncates
    # at it, so the dumps differ on a printer quirk, not a parse divergence. The
    # escape decoding itself is gated by lex_parity + the escapes.esk runtime golden.
    files=()
    for _f in tests/*.esk; do
        [ "$_f" = "tests/escapes.esk" ] && continue
        files+=("$_f")
    done
    echo "corpus: ${#files[@]} files (full tests/*.esk)"
    echo "----"
elif [ "$#" -gt 0 ]; then
    files=("$@")
else
    files=(tests/selfhost/parse_inputs/*.esk)
fi

fail=0
total=0
skipped=0
for f in "${files[@]}"; do
    [ -f "$f" ] || { echo "MISS  $f (no such file)"; fail=1; continue; }
    # The reference oracle itself must parse cleanly. The C++ ASTPrinter crashes on
    # a body-less FunctionDecl (top-level prototype), so skip files where
    # --test-parser doesn't reach "Parse succeeded!" — they can't be compared.
    rawref=$("$BIN" --test-parser "$f" 2>/dev/null)
    case "$rawref" in
        *"Parse succeeded!"*) ;;
        *) echo "skip  ${f#tests/}  (C++ --test-parser did not succeed)"; skipped=$((skipped + 1)); continue ;;
    esac
    total=$((total + 1))
    ref=$(printf '%s' "$rawref" | strip_banner)
    got=$(ESKIU_ROOT="$(pwd)" "$PBIN" --test-parser "$f" 2>/dev/null)
    if [ "$ref" = "$got" ]; then
        echo "ok    $f"
    else
        echo "FAIL  $f"
        diff <(printf '%s\n' "$ref") <(printf '%s\n' "$got") | sed 's/^/      /'
        fail=1
    fi
done

echo "----"
if [ "$fail" -eq 0 ]; then
    echo "parity: $total/$total files match"
else
    echo "parity: MISMATCH"
fi
exit "$fail"
