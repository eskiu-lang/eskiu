#!/usr/bin/env bash
# ir_golden.sh — structural LLVM IR assertions for critical ABI & codegen invariants
#
# Complements behavioral testing by checking that emitted LLVM IR contains the exact
# structural attributes required by the target C ABI and LLVM verification:
#   - `sret` attributes on large aggregate returns
#   - `signext` / `zeroext` on narrow integer parameters and return types
#   - `personality` function and `landingpad` on exception blocks
#   - `@__cabi_*` callback thunks for C function pointers
#   - `noundef` on scalar parameters and results, `nounwind` only where nothing can throw
#
# Usage: ./tests/ir_golden.sh [path-to-eskiuc]

set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/.." && pwd)"

ESKIUC="${1:-${ESKIUC:-$repo/build/eskiuc}}"
if [[ ! -x "$ESKIUC" ]]; then
    echo "error: compiler not found at $ESKIUC (build it first: cmake --build build)" >&2
    exit 2
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

pass=0
fail=0
failed_names=()

ok()  { printf '  \033[32mPASS\033[0m  %s\n' "$1"; pass=$((pass+1)); }
bad() { printf '  \033[31mFAIL\033[0m  %s — %s\n' "$1" "$2"; fail=$((fail+1)); failed_names+=("$1"); }

echo "Structural LLVM IR assertions:"

# 1. sret on large aggregate return across targets
for target in "arm64-apple-darwin" "x86_64-unknown-linux-gnu" "x86_64-apple-darwin"; do
    ir="$work/struct_$target.ll"
    if "$ESKIUC" "$here/c_abi_struct.esk" --test-codegen --target "$target" > "$ir" 2>&1; then
        if grep -q "sret(%Big)" "$ir" && grep -q "call void @c_mk(ptr sret(%Big)" "$ir"; then
            ok "ir/sret ($target)"
        else
            bad "ir/sret ($target)" "missing sret attribute on %Big return"
        fi
    else
        bad "ir/sret ($target)" "codegen failed"
    fi
done

# 2. signext and zeroext on narrow integer ABI boundaries
ir="$work/narrow.ll"
if "$ESKIUC" "$here/c_abi_narrow.esk" --test-codegen > "$ir" 2>&1; then
    if grep -q "declare i64 @c_s8(i8 signext)" "$ir" && \
       grep -q "declare i64 @c_u8(i8 zeroext)" "$ir" && \
       grep -q "declare signext i8 @c_ret_s8(i32)" "$ir" && \
       grep -q "declare zeroext i16 @c_ret_u16(i32)" "$ir"; then
        ok "ir/signext_zeroext"
    else
        bad "ir/signext_zeroext" "missing signext/zeroext parameter or return attributes"
    fi
else
    bad "ir/signext_zeroext" "codegen failed"
fi

# 3. Exception landingpad and personality function
ir="$work/try.ll"
if "$ESKIUC" "$here/c_abi_try.esk" --test-codegen > "$ir" 2>&1; then
    if grep -q "personality ptr @__gxx_personality_v0" "$ir" && \
       grep -q "landingpad" "$ir" && \
       grep -q "__gxx_personality_v0" "$ir"; then
        ok "ir/eh_personality"
    else
        bad "ir/eh_personality" "missing __gxx_personality_v0 or landingpad in IR"
    fi
else
    bad "ir/eh_personality" "codegen failed"
fi

# 4. C ABI callback thunks
ir="$work/callback.ll"
if "$ESKIUC" "$here/c_abi_callback.esk" --test-codegen > "$ir" 2>&1; then
    if grep -q "@__cabi_" "$ir"; then
        ok "ir/cabi_callback_thunks"
    else
        bad "ir/cabi_callback_thunks" "missing @__cabi_ thunk in IR"
    fi
else
    bad "ir/cabi_callback_thunks" "codegen failed"
fi

# 5. noundef on scalar parameters and results; nounwind on a function that cannot throw
# (a leaf, and its caller), not on one that throws, calls a C function (which may call
# back into code that throws) or calls through a pointer.
ir="$work/attrs.ll"
cat > "$work/attrs.esk" <<'EOF'
extern int puts(string s);
int leaf(int a, double b, *int p) { return a + (int)b + *p; }
int calls_leaf(int a) { int v = 1; return leaf(a, 2.0, &v); }
int throws(int a) { if (a > 3) { throw a; } return a; }
int calls_c(string s) { return puts(s); }
int indirect(fn(int)->int f) { return f(1); }
struct Big { int64 a; int64 b; int64 c; }
Big big(int x) { Big r; r.a = (int64)x; return r; }
int leaf1(int x) { return x; }
int main() { return calls_leaf(1) + throws(1) + calls_c("x") + indirect(leaf1) + (int)big(1).a; }
EOF
if "$ESKIUC" "$work/attrs.esk" --test-codegen > "$ir" 2>&1; then
    nounwind() {
        local g
        g="$(grep "^define .*@$1(" "$ir" | grep -o '#[0-9]*' | tr -d '#')"
        [[ -n "$g" ]] && grep "^attributes #$g = " "$ir" | grep -q nounwind
    }
    if grep -q "^define noundef i32 @leaf(i32 noundef %a, double noundef %b, ptr noundef %p)" "$ir" &&
            grep -q "^define void @big(ptr %sret.ptr, i32 noundef %x)" "$ir"; then
        ok "ir/noundef"
    else
        bad "ir/noundef" "$(grep -E '^define .*@(leaf|big)\(' "$ir")"
    fi
    if nounwind leaf && nounwind calls_leaf && ! nounwind throws && ! nounwind calls_c &&
            ! nounwind indirect && ! nounwind main; then
        ok "ir/nounwind"
    else
        bad "ir/nounwind" "$(grep -E '^(define|attributes)' "$ir" | tr '\n' ' ')"
    fi
else
    bad "ir/noundef" "codegen failed: $(head -3 "$ir")"
fi

echo ""
echo "Summary: $pass passed, $fail failed."
if [[ $fail -gt 0 ]]; then
    printf 'Failed tests: %s\n' "${failed_names[*]}" >&2
    exit 1
fi
exit 0
