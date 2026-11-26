#!/usr/bin/env bash
# safe_parity.sh: behavioral parity for the `--safe` run-time checks.
#
# The self-hosted back-end (selfhost/codegen.esk) must insert the same runtime
# checks as the C++ `eskiuc` under `--safe` and `--safe=LIST` (bounds, overflow,
# null): an out-of-range index fails, an in-bounds one does not, each check is on
# only with its category, and without `--safe` there is no check.
# For each program this builds a native binary with BOTH compilers and asserts the
# exit codes agree (a failed check aborts: the same signal exit on both), and for the
# programs at the end that the one-line report on stderr is byte-identical and as expected.
#
# Usage: tests/selfhost/safe_parity.sh   (from repo root or anywhere)
set -u
cd "$(dirname "$0")/../.." || exit 2

BIN="${ESKIUC:-}"
if [ -z "$BIN" ]; then
    if [ -x build/eskiuc ]; then BIN=build/eskiuc
    elif command -v eskiuc >/dev/null 2>&1; then BIN=eskiuc
    else echo "safe_parity: cannot find eskiuc (set ESKIUC or build it)"; exit 2; fi
fi
case "$BIN" in */*) BIN="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")" ;; esac
ROOT="$(pwd)"
CC="${CC:-cc}"
DRIVER=selfhost/esk_main.esk
ESKMAIN="$(mktemp -t esk_main.XXXXXX)"
WORK="$(mktemp -d)"
trap 'rm -f "$ESKMAIN"; rm -rf "$WORK"' EXIT

# Build the self-hosted compiler with the C++ seed.
if ! ESKIU_ROOT="$(pwd)" "$BIN" "$DRIVER" -o "$ESKMAIN" >/dev/null 2>"$WORK/build.log"; then
    echo "safe_parity: failed to build $DRIVER"; cat "$WORK/build.log"; exit 2
fi

fail=0
# The C++ eskiuc emits an object (link it with $CC); the self-hosted esk_main's `-o`
# invokes clang itself and writes a native binary. Compile $src with flag $flag, run the
# result, echo the exit code. $self=1 selects the self-hosted (already-linked) path.
run() { # comp src flag self
    local comp="$1" src="$2" flag="${3:-}" self="${4:-0}"
    if [ "$self" = 1 ]; then
        ESKIU_ROOT="$(pwd)" "$comp" $flag "$src" -o "$WORK/t" >/dev/null 2>&1 || { echo "compile-fail"; return; }
    else
        ESKIU_ROOT="$(pwd)" "$comp" $flag "$src" -o "$WORK/t.o" >/dev/null 2>&1 || { echo "compile-fail"; return; }
        $CC "$WORK/t.o" -o "$WORK/t" >/dev/null 2>&1 || { echo "link-fail"; return; }
    fi
    "$WORK/t" >/dev/null 2>&1; echo "$?"
}
# Assert both compilers give the same exit code for $src under flag $flag.
parity() { # name src flag
    local name="$1" src="$2" flag="${3:-}"
    local cpp self
    cpp="$(run "$BIN" "$src" "$flag" 0)"
    self="$(run "$ESKMAIN" "$src" "$flag" 1)"
    if [ "$cpp" = "$self" ]; then echo "  ok    $name  (exit $cpp)"; else
        echo "  FAIL  $name  (cpp=$cpp self=$self)"; fail=1; fi
}

cat > "$WORK/slice_oob.esk" <<'EOF'
int main() { int[5] a = {1,2,3,4,5}; int[] s = a[1..4]; int i = 9; return s[i]; }
EOF
cat > "$WORK/array_oob.esk" <<'EOF'
int main() { int[5] a = {1,2,3,4,5}; int i = 7; return a[i]; }
EOF
cat > "$WORK/inbounds.esk" <<'EOF'
int main() { int[5] a = {1,2,3,4,5}; int[] s = a[1..4]; int i = 2; return s[i]; }
EOF
cat > "$WORK/slice_order.esk" <<'EOF'
int main() { int[5] a = {1,2,3,4,5}; int[] s = a[0..5]; int lo = 3; int hi = 1; int[] t = s[lo..hi]; return (int)t.len; }
EOF
cat > "$WORK/default_noop.esk" <<'EOF'
int main() { int[5] a = {1,2,3,4,5}; int[] s = a[1..4]; int i = 9; int x = s[i]; return 0; }
EOF

parity "slice OOB traps (--safe)"     "$WORK/slice_oob.esk"    --safe
parity "array OOB traps (--safe)"     "$WORK/array_oob.esk"    --safe
parity "in-bounds ok (--safe)"        "$WORK/inbounds.esk"     --safe
parity "slice of slice lo > hi traps"  "$WORK/slice_order.esk"  --safe
parity "no check without --safe"      "$WORK/default_noop.esk" ""

cat > "$WORK/overflow.esk" <<'EOF'
int main() { int a = 2147483647; int b = a; b = b - 2147483646; return a + b == -2147483647 - 1 ? 0 : 1; }
EOF
cat > "$WORK/nullderef.esk" <<'EOF'
int main() { *int p = null; return *p; }
EOF
cat > "$WORK/array_oob_read.esk" <<'EOF'
int main() { int[5] a = {1,2,3,4,5}; int i = 7; int x = a[i]; return 0; }
EOF
for c in bounds overflow null bounds,null overflow,null,bounds; do
    parity "overflow (--safe=$c)"  "$WORK/overflow.esk"       "--safe=$c"
    parity "null deref (--safe=$c)" "$WORK/nullderef.esk"     "--safe=$c"
    parity "array OOB (--safe=$c)" "$WORK/array_oob_read.esk" "--safe=$c"
done
# An unknown name: the same driver error from both.
cpp_msg="$(ESKIU_ROOT="$(pwd)" "$BIN" --safe=null,bogus "$WORK/overflow.esk" -o "$WORK/t.o" 2>&1; echo "rc=$?")"
self_msg="$(ESKIU_ROOT="$(pwd)" "$ESKMAIN" --safe=null,bogus "$WORK/overflow.esk" -o "$WORK/t" 2>&1; echo "rc=$?")"
if [ "$cpp_msg" = "$self_msg" ]; then echo "  ok    unknown --safe name  ($cpp_msg)" | tr '\n' ' '; echo; else
    echo "  FAIL  unknown --safe name"; echo "    cpp:  $cpp_msg"; echo "    self: $self_msg"; fail=1; fi

# The report of a failed check: `FILE:LINE:COL: message` on stderr, then abort. Each
# program is built in $WORK from its bare name, so FILE is that name; both compilers
# must print the expected line exactly, and the program must exit non-zero.
run_err() { # comp src self -> "exit|stderr"
    local comp="$1" src="$2" self="$3" out code
    if [ "$self" = 1 ]; then
        (cd "$WORK" && ESKIU_ROOT="$ROOT" "$comp" --safe "$src" -o t >/dev/null 2>&1) || { echo "compile-fail"; return; }
    else
        (cd "$WORK" && ESKIU_ROOT="$ROOT" "$comp" --safe "$src" -o t.o >/dev/null 2>&1) || { echo "compile-fail"; return; }
        $CC "$WORK/t.o" -o "$WORK/t" >/dev/null 2>&1 || { echo "link-fail"; return; }
    fi
    out="$(cd "$WORK" && ./t 2>&1 >/dev/null)"; code=$?
    echo "$code|$out"
}
report() { # name file expected-stderr
    local name="$1" src="$2" want="$3" cpp self
    cpp="$(run_err "$BIN" "$src" 0)"
    self="$(run_err "$ESKMAIN" "$src" 1)"
    if [ "$cpp" != "$self" ]; then echo "  FAIL  $name"; echo "    cpp:  $cpp"; echo "    self: $self"; fail=1
    elif [ "${cpp#*|}" != "$want" ] || [ "${cpp%%|*}" = 0 ]; then echo "  FAIL  $name (got: $cpp)"; fail=1
    else echo "  ok    $name  ($want)"; fi
}
mk() { cat > "$WORK/$1"; }
mk r_index.esk <<'EOF'
int get(int[] s, int i) {
    return s[i];
}
int main() { int[4] a = {1, 2, 3, 4}; return get(a[0..4], 7); }
EOF
report "array index"      r_index.esk 'r_index.esk:2:13: index 7 out of bounds for length 4'
mk r_neg.esk <<'EOF'
int main() { int[3] a = {1, 2, 3}; int i = -1; return a[i]; }
EOF
report "negative index"   r_neg.esk   'r_neg.esk:1:56: index -1 out of bounds for length 3'
mk r_range.esk <<'EOF'
int main() { int[5] a = {1,2,3,4,5}; int hi = 9; int[] s = a[2..hi]; return (int)s.len; }
EOF
report "slice range"      r_range.esk 'r_range.esk:1:61: slice [2..9] out of bounds for length 5'
mk r_order.esk <<'EOF'
int main() { int[5] a = {1,2,3,4,5}; *int p = &a[0]; int lo = 3; int[] s = p[lo..1]; return (int)s.len; }
EOF
report "pointer slice"    r_order.esk 'r_order.esk:1:77: slice [3..1] out of bounds'
mk r_add.esk <<'EOF'
int main() { int a = 2147483647; int b = 1; return a + b; }
EOF
report "overflow +"       r_add.esk   "r_add.esk:1:54: integer overflow in '+'"
mk r_mul.esk <<'EOF'
int64 sq(int64 x) { return x * x; }
int main() { return (int)sq((int64)4000000000 * 1000); }
EOF
report "overflow * int64" r_mul.esk   "r_mul.esk:1:30: integer overflow in '*'"
mk r_cmpd.esk <<'EOF'
int k = 0;
int f() { k = k + 1; return 0; }
int main() { int[2] a = {2147483647, 0}; a[f()] -= -1; return a[0]; }
EOF
report "compound -="      r_cmpd.esk  "r_cmpd.esk:3:49: integer overflow in '-'"
mk r_null.esk <<'EOF'
struct Node { int v; *Node next; }
int main() { Node n = Node{1, null}; *Node p = &n; return p.next.v; }
EOF
report "null p.f"         r_null.esk  'r_null.esk:2:65: null pointer dereference'
mk r_store.esk <<'EOF'
int main() { *int p = null; *p = 3; return 0; }
EOF
report "null *p = v"      r_store.esk 'r_store.esk:1:29: null pointer dereference'
mk r_pidx.esk <<'EOF'
int main() { *int p = null; return p[2]; }
EOF
report "null p[i]"        r_pidx.esk  'r_pidx.esk:1:37: null pointer dereference'
mk lib_safe.esk <<'EOF'
struct Box<T> { T[4] items;
    T at(int i) { return self.items[i]; }
}
int nth(int[] s, int i) { return s[i]; }
EOF
mk r_import.esk <<'EOF'
import "lib_safe.esk";
int main() { int[2] a = {1, 2}; return nth(a[0..2], 5); }
EOF
report "in an import"     r_import.esk './lib_safe.esk:4:35: index 5 out of bounds for length 2'
mk r_gmeth.esk <<'EOF'
import "lib_safe.esk";
int main() { Box<int> b; return b.at(9); }
EOF
report "generic method"   r_gmeth.esk './lib_safe.esk:2:36: index 9 out of bounds for length 4'
mk r_lambda.esk <<'EOF'
int main() {
    int[3] a = {1, 2, 3};
    fn(int) -> int f = int(int i) { return a[i]; };
    return f(3);
}
EOF
report "in a lambda"      r_lambda.esk 'r_lambda.esk:3:45: index 3 out of bounds for length 3'

echo "----"
[ "$fail" = 0 ] && echo "safe parity: OK" || echo "safe parity: MISMATCH"
exit "$fail"
