#!/usr/bin/env bash
# Gate for the integrated ARM64 assembler (eir/eir_as_arm64.cpp): for each run test
# (tests/NAME.esk with a NAME.expected), the assembly `eskiuc --emit-asm` writes is assembled
# twice, by the system C driver (`cc -c`) and by the integrated assembler
# (`eskiuc --eir-assemble`), and the two objects must match: disassembly with raw encodings
# and inline relocations, every relocation record, the defined and undefined symbols, and the
# section names, sizes and addresses. What legitimately differs is normalized away: the
# section-start symbols LLVM adds on Mach-O (ltmpN), the label next to each address, the
# order of the symbol table and of the ELF section headers, and the section-table-only ELF
# sections (.strtab, .symtab, .rela.*, and the .note.GNU-stack the integrated assembler adds).
#
#   tests/eir/obj_check.sh [macho|elf|both] [NAME...]
#
# macho: the host's Mach-O (needs an arm64 Mac); elf: --target aarch64-unknown-linux-gnu,
# assembled by `cc -target ...` (no linking). Default: both. ESKIUC picks the compiler,
# OBJDUMP the llvm-objdump (default: the one next to `brew --prefix llvm@22`, else on PATH).

set -u
export LC_ALL=C
cd "$(dirname "$0")/../.." || exit 2

BIN="${ESKIUC:-build/eskiuc}"
[ -x "$BIN" ] || { echo "eir/obj_check: cannot find eskiuc (set ESKIUC or build it)"; exit 2; }
CC="${CC:-cc}"
OBJDUMP="${OBJDUMP:-}"
if [ -z "$OBJDUMP" ]; then
    if command -v brew >/dev/null 2>&1 && [ -x "$(brew --prefix llvm@22 2>/dev/null)/bin/llvm-objdump" ]; then
        OBJDUMP="$(brew --prefix llvm@22)/bin/llvm-objdump"
    else
        OBJDUMP="$(command -v llvm-objdump || true)"
    fi
fi
[ -x "$OBJDUMP" ] || { echo "eir/obj_check: cannot find llvm-objdump (set OBJDUMP)"; exit 2; }

mode="${1:-both}"
case "$mode" in macho|elf|both) shift ;; *) mode=both ;; esac
formats=()
[ "$mode" = macho ] || [ "$mode" = both ] && formats+=(macho)
[ "$mode" = elf ] || [ "$mode" = both ] && formats+=(elf)

names=("$@")
if [ ${#names[@]} -eq 0 ]; then
    for f in tests/*.esk; do
        n="$(basename "$f" .esk)"
        [ -f "tests/$n.expected" ] && names+=("$n")
    done
fi

work="$(mktemp -d -t eskiu_objcheck.XXXXXX)"
trap 'rm -rf "$work"' EXIT

# One comparable text of an object.
describe() {
    local o="$1"
    echo "== sections"
    "$OBJDUMP" -h "$o" | awk 'NR > 4 && NF >= 4 && $2 !~ /^(\.strtab|\.symtab|\.note\.GNU-stack|\.rela.*)$/ { print $2, $3, $4 }'
    echo "== symbols"
    "$OBJDUMP" -t "$o" | awk 'NR > 3 && NF > 0 && $NF !~ /^ltmp[0-9]+$/' | sort
    echo "== relocations"
    "$OBJDUMP" -r "$o" | awk 'NR > 2'
    echo "== code"
    "$OBJDUMP" -d -r "$o" | awk 'NR > 2 && $0 !~ /^[0-9a-f]+ <.*>:$/ && NF > 0' | sed -E 's/ <[^>]*>//'
    echo "== data"
    "$OBJDUMP" -s "$o" | awk '
        /^Contents of section / { keep = ($NF !~ /^(\.strtab|\.symtab|\.note\.GNU-stack|\.rela.*):$/) }
        NR > 2 && keep'
}

pass=0
fails=()
for fmt in "${formats[@]}"; do
    tflag=()
    ccflag=()
    if [ "$fmt" = elf ]; then
        tflag=(--target aarch64-unknown-linux-gnu)
        ccflag=(-target aarch64-unknown-linux-gnu)
    fi
    for n in "${names[@]}"; do
        s="$work/$n.$fmt.s"
        if ! "$BIN" "tests/$n.esk" "${tflag[@]+"${tflag[@]}"}" --emit-asm -o "$s" >"$work/err" 2>&1; then
            fails+=("$fmt $n: --emit-asm failed: $(head -1 "$work/err")")
            continue
        fi
        if ! "$CC" "${ccflag[@]+"${ccflag[@]}"}" -c "$s" -o "$work/ref.o" >"$work/err" 2>&1; then
            fails+=("$fmt $n: cc -c failed: $(grep -m1 error "$work/err")")
            continue
        fi
        if ! "$BIN" "$s" "${tflag[@]+"${tflag[@]}"}" --eir-assemble -o "$work/our.o" >"$work/err" 2>&1; then
            fails+=("$fmt $n: integrated assembler: $(head -1 "$work/err")")
            continue
        fi
        describe "$work/ref.o" > "$work/ref.txt"
        describe "$work/our.o" > "$work/our.txt"
        if cmp -s "$work/ref.txt" "$work/our.txt"; then
            pass=$((pass + 1))
        else
            fails+=("$fmt $n: objects differ: $(diff "$work/ref.txt" "$work/our.txt" | grep -m2 '^[<>]' | tr '\n' ' ')")
            [ -n "${KEEP:-}" ] && cp "$work/ref.txt" "$KEEP.$fmt.$n.ref" && cp "$work/our.txt" "$KEEP.$fmt.$n.our"
        fi
    done
done

total=$(( ${#names[@]} * ${#formats[@]} ))
if [ ${#fails[@]} -gt 0 ]; then
    printf '%s\n' "${fails[@]}"
    echo "eir/obj_check: ${#fails[@]} of $total differ"
    exit 1
fi
echo "eir/obj_check (${formats[*]}): $pass of $total objects match"
