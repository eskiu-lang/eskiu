#!/usr/bin/env bash
# Parity gate for `--diagnostics=json` (docs/dev/diagnostics.md).
#
#   * Byte-exact: the self-hosted driver (selfhost/esk_main.esk) prints the same JSON
#     lines as eskiuc for every file of tests/errors/, tests/warnings/ (with no flags,
#     -Wall -Wextra and -Werror -Wall) and tests/diag/ (its DIAG-FLAGS), through
#     --test-typechecker. The files in TEXT_DIFFERS below are those whose diagnostics
#     already differ in the text form (a type spelled `int*` for `*int`, a shorter
#     message, an instantiation note): the JSON is the text form converted, so they are
#     listed, and a listed file that comes to agree fails until it is removed.
#   * Same set: eskiuc's JSON has one error or warning object per error or warning line
#     of its text form.
#   * Ids: the ids eskiuc gives over this corpus are the list in
#     docs/dev/diagnostic-ids.txt (UPDATE=1 rewrites it).
#
# Green (exit 0) = all three hold.

set -u
cd "$(dirname "$0")/../.." || exit 2

BIN="${ESKIUC:-}"
if [ -z "$BIN" ]; then
    if [ -x build/eskiuc ]; then BIN=build/eskiuc
    elif command -v eskiuc >/dev/null 2>&1; then BIN=eskiuc
    else echo "diag_parity: cannot find eskiuc (set ESKIUC or build it)"; exit 2; fi
fi

TEXT_DIFFERS=" addr_of_bitfield array_dim_cast_zero array_dim_sizeof_zero array_size_zero asm_output_const
async_no_await bitfield_unnamed_negative cast_int_to_struct_generic const_addr_arg const_addr_of
const_array_slice const_field const_method_call const_param_assign const_ptr_deref_method
const_ptr_drop const_ptr_index_write const_ptr_member_write const_ptr_method_call
const_ptr_nested_field_write const_ptr_ternary const_ptr_write const_reassign const_self_write
const_struct_addr dup_struct_fn dup_struct export_async generic_dot_call_const
generic_iface_missing generic_inst_method_arg generic_inst_nested generic_inst_operator
generic_ptr_arg_mismatch generic_struct_method_inst generic_uninit_read generic_void_param
global_forward_ref global_init_const_drop iface_arity_mismatch iface_call_arg_count
iface_call_arg_type iface_compare_ptr iface_const_receiver iface_from_int_ptr iface_from_nullable
iface_ptr_to_ptr iface_return_mismatch iface_ternary_nonconforming import_missing_std
interface_by_value keyword_field lambda_const_capture_assign lambda_question_non_result
lambda_sig_mismatch lambda_void_returns_value list_join_non_string match_dup_default
match_on_enum_pointer match_on_struct method_arg_type method_free_fn_arg_type
method_on_generic_prim operator_ptr_arg_mismatch parse_array_field proto_conflict
ptr_conv_struct_sum ptr_conv_sum_int ptr_conv_sum_struct question_bad_return question_type_args
return_in_void struct_lit_generic_ptr_field ternary_incompatible ternary_unrelated_ptrs
unknown_intrinsic void_ptr_deref_value "
TEXT_DIFFERS="$(echo $TEXT_DIFFERS)"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
DRV="$work/esk_main"
if ! "$BIN" selfhost/esk_main.esk -o "$DRV" >/dev/null 2>"$work/build.log"; then
    echo "diag_parity: failed to build selfhost/esk_main.esk"; cat "$work/build.log"; exit 2
fi

fail=0
same=0
listed=0
: > "$work/ids"

check() {   # check NAME FILE FLAGS...
    local name="$1" f="$2"; shift 2
    local ref got text n_text n_json
    ref="$("$BIN" --diagnostics=json --test-typechecker "$@" "$f" 2>&1 >/dev/null)"
    got="$(ESKIU_ROOT="$(pwd)" "$DRV" --diagnostics=json --test-typechecker "$@" "$f" 2>&1 >/dev/null)"
    printf '%s\n' "$ref" | sed -n -E 's/^\{"severity":"[a-z]+","id":"([^"]*)".*/\1/p' >> "$work/ids"
    text="$("$BIN" --test-typechecker "$@" "$f" 2>&1 >/dev/null)"
    n_text="$(printf '%s\n' "$text" | grep -cE '^(error|warning): |^[^ ]*:[0-9]+:[0-9]+: (error|warning): ')"
    n_json="$(printf '%s\n' "$ref" | grep -cE '^\{"severity":"(error|warning)"')"
    if [ "$n_text" != "$n_json" ]; then
        echo "  FAIL  $f $*  ($n_text diagnostics in the text form, $n_json JSON objects)"; fail=1
    fi
    case " $TEXT_DIFFERS " in
        *" $name "*)
            if [ "$ref" = "$got" ]; then
                echo "  FAIL  $f $*  (now agrees: remove '$name' from TEXT_DIFFERS)"; fail=1
            else
                listed=$((listed + 1))
            fi
            return ;;
    esac
    if [ "$ref" = "$got" ]; then
        same=$((same + 1))
    else
        echo "  FAIL  $f $*"
        diff <(printf '%s\n' "$ref") <(printf '%s\n' "$got") | sed 's/^/    /' | head -10
        fail=1
    fi
}

echo "JSON diagnostics (byte-exact):"
for f in tests/errors/*.esk; do
    check "$(basename "$f" .esk)" "$f"
done
for f in tests/warnings/*.esk; do
    check "w_$(basename "$f" .esk)" "$f"
    check "w_$(basename "$f" .esk)" "$f" -Wall -Wextra
    check "w_$(basename "$f" .esk)" "$f" -Werror -Wall
done
for f in tests/diag/*.esk; do
    flags="$(head -1 "$f" | sed -n 's|^// DIAG-FLAGS:[[:space:]]*||p')"
    # shellcheck disable=SC2086
    check "d_$(basename "$f" .esk)" "$f" $flags
done
echo "  ok: $same runs agree; $listed listed in TEXT_DIFFERS (the text form differs)"

echo "Diagnostic ids (docs/dev/diagnostic-ids.txt):"
LIST=docs/dev/diagnostic-ids.txt
sort -u "$work/ids" > "$work/ids.sorted"
if [ "${UPDATE:-0}" = 1 ]; then
    {
        echo "# The diagnostic ids the test corpus produces (tests/selfhost/diag_parity.sh;"
        echo "# UPDATE=1 tests/selfhost/diag_parity.sh rewrites this file). How an id is"
        echo "# made from its message: docs/dev/diagnostics.md."
        cat "$work/ids.sorted"
    } > "$LIST"
    echo "  wrote $LIST ($(wc -l < "$work/ids.sorted" | tr -d ' ') ids)"
elif ! diff <(grep -v '^#' "$LIST") "$work/ids.sorted" > "$work/ids.diff"; then
    echo "  FAIL  the corpus ids differ from $LIST (UPDATE=1 rewrites it):"
    sed 's/^/    /' "$work/ids.diff" | head -20
    fail=1
else
    echo "  ok: $(wc -l < "$work/ids.sorted" | tr -d ' ') ids"
fi

echo "----"
if [ "$fail" -eq 0 ]; then echo "diag parity: OK"; else echo "diag parity: MISMATCH"; fi
exit "$fail"
