#!/usr/bin/env bash
# Parity gate for the self-hosted type checker (selfhost/sema.esk) vs the C++ one.
#
# Sema has no byte-exact dump like the lexer/parser; the contract is the VERDICT.
# So, mirroring tests/run.sh's negative tests:
#   * Positive corpus (tests/*.esk): the self-hosted `tc_main` must reach the same
#     verdict as `eskiuc --test-typechecker` (all currently type-check clean → both
#     exit 0). This guards against the checker FALSELY rejecting valid code.
#   * Negative corpus (tests/errors/*.esk): for the error classes listed in HANDLED
#     below, `tc_main` must reject (exit != 0) AND emit the
#     file's `EXPECT-ERROR:` substring, at the same line:col as the C++ checker (the
#     first error line carrying that text in each output). The other files are listed
#     as skipped.
#
# Green (exit 0) = verdict parity on every positive file + every handled negative.

set -u
cd "$(dirname "$0")/../.." || exit 2

BIN="${ESKIUC:-}"
if [ -z "$BIN" ]; then
    if [ -x build/eskiuc ]; then BIN=build/eskiuc
    elif command -v eskiuc >/dev/null 2>&1; then BIN=eskiuc
    else echo "tc_parity: cannot find eskiuc (set ESKIUC or build it)"; exit 2; fi
fi

# Error classes (files under tests/errors/) checked against the C++ compiler.
HANDLED="undefined_var arg_count undefined_type await_outside_async async_no_await switch_dup_case switch_dup_folded switch_dup_const unknown_intrinsic undefined_field match_duplicate match_nonexhaustive
         dup_enum_member_located bitfield_located alias_unknown_type missing_return_located uninit_located undefined_fn_located struct_redef_located int64_literal_range enum_value_overflow enum_value_too_big switch_case_range switch_case_range_int shift_count_range shift_count_negative index_oob_expr index_oob_enum index_oob_const struct_init_mixed array_size_negative array_size_zero compare_slice compare_array compare_iface compare_closure not_slice logical_struct slice_len_assign method_value_self method_wrong_self iface_ptr_to_ptr iface_from_nullable method_on_nullable iface_const_receiver nullable_cond_assign nullable_ptr_arith const_param_assign const_array_slice dangling_element dangling_field dangling_slice generic_redefinition generic_nongeneric_clash generic_proto_clash struct_value_cycle_array call_shadowed_fn method_ptr_ptr operator_on_primitives operator_on_pointers dup_type_param dup_iface_method extern_conflict match_dup_binding variant_struct_clash must_use_ternary must_use_for_step must_use_operator must_use_void main_bad_params lambda_missing_return async_main
         void_logical_operand void_compare void_variadic_arg
         closure_captured_method_call closure_captured_method_field closure_captured_method_generic closure_captured_method_inline
         bitfield_generic_float
         const_reassign const_no_init const_field const_ptr_write const_ptr_drop
         trait_unsatisfied trait_primitive_unsat question_bad_return escaping_param
         missing_return missing_return_if
         main_void redefinition defer_return defer_break div_by_zero incdec_nonlvalue
         labeled_break_unknown labeled_break_defer
         trailing_block_return trailing_block_break trailing_block_continue trailing_block_await trailing_block_question
         catch_any_not_last exception_text_outside_catch exception_text_in_lambda exception_text_args
         trailing_block_not_call trailing_block_parens trailing_block_call_result trailing_block_initializer trailing_block_return_value
         const_addr_of init_incompatible init_void float_to_int literal_out_of_range
         compare_incompatible compare_struct ternary_incompatible fn_return_mismatch
         index_oob array_overflow array_2d_oob array_2d_init_overflow
         dangling_local uninitialized pp_error unterminated_char
         incdec_float question_type_args c_array_global c_array_local
         struct_init_field_type struct_init_too_many struct_init_dup_field struct_init_lit_range
         struct_init_generic_field sizeof_unknown
         struct_value_cycle field_unknown_type param_unknown_type template_arg_count template_unknown_arg
         iface_return_mismatch iface_arity_mismatch arg_type_mismatch
         missing_return_labeled_break missing_return_dowhile_break missing_return_switch_break
         nullable_reassign nullable_shadow nullable_loop_reassign
         const_method_call const_ptr_method_call const_addr_arg const_struct_addr must_use_method
         fn_value_arg_count fn_value_arg_type fn_field_arg_count fn_chained_arg_count iface_call_arg_count
         generic_arg_count generic_explicit_arg_count generic_type_arg_count generic_arg_conflict generic_uninferable generic_infer_shape
         match_dup_value alias_cycle void_var void_param void_field bitfield_float bitfield_too_wide
         operator_redefinition
         dup_local dup_param_local dup_global dup_param dup_fn_global dup_struct_fn dup_struct dup_field dup_enum
         dup_enum_member dup_default dup_method_fn dup_field_method proto_conflict
         slice_bounds_order slice_bounds_oob compound_lit_range ternary_lit_range
         assign_rvalue assign_ternary addr_of_call addr_of_literal addr_of_bitfield index_scalar
         index_struct cast_struct cast_ptr_float case_nonconst break_outside continue_in_switch
         await_in_lambda unknown_sig_type unknown_var_type
         init_string_from_int init_ptr_from_int assign_float_to_int float_expr_to_int match_non_enum
         lambda_sig_mismatch question_non_result undefined_template_fn
         return_in_void call_non_fn member_of_int shift_by_float string_plus_string
         pp_include pp_unknown_directive pp_stray_endif pp_stray_else pp_missing_endif
         pp_if_bad_expr pp_stringify pp_if_zero_div
         pp_if_shift_range pp_if_int_too_large pp_if_multichar
         pp_macro_too_few pp_macro_too_many
         import_lex_error import_directory import_empty
         int_literal_too_large hex_literal_too_large float_exponent_no_digits
         float_const_cast_range
         unexpected_char empty_char unterminated_comment unterminated_string
         octal_bad_digit octal_bad_digit2 hex_no_digits number_suffix number_underscore
         parse_error parse_error_located parse_missing_operand parse_return_no_semi parse_import_no_semi
         parse_catch_colon parse_array_field parse_postdec_literal parse_leading_dot
         import_missing import_missing_std operator_as_name
         pragma_link_malformed pragma_pack_invalid pragma_pack_push_invalid method_arg_type method_arg_count method_free_fn_arg_type iface_call_arg_type generic_inst_method_arg
         generic_inst_operator generic_inst_nested generic_struct_method_inst extern_fnptr_closure
         range_bound_float range_bound_ptr
         generic_dot_call_arg_type generic_dot_call_arg_count generic_dot_call_const generic_dot_call_in_generic
         call_global_nonfn unknown_global_type deref_non_pointer unary_struct string_arith struct_arith
         return_no_value assign_struct_to_int switch_case_location
         nullable_to_ptr_arg nullable_to_ptr_return nullable_to_ptr_assign operator_void_result hex_literal_range hex_literal_range_wide literal_over_64_bits struct_lit_other_struct array_assign_size array_arg_size slice_to_ptr slice_elem_mismatch fn_value_null async_result_as_int await_result_mismatch iface_to_iface iface_method_ret_mismatch iface_from_int_ptr generic_explicit_arg_type generic_ret_mismatch const_ptr_ternary void_ptr_deref_value
         lambda_return_mismatch lambda_void_returns_value lambda_undefined_var lambda_const_capture_assign lambda_dangling_local lambda_dup_param lambda_question_non_result cond_fn_value cond_string cond_struct_while cond_enum_payload ternary_cond_struct fn_value_arith fn_value_deref bnot_float operator_unary_as_binary operator_index_assign operator_operand_order incdec_struct compare_union compare_enum_payload array_plus enum_payload_arith union_unknown_member
         match_payload_arity match_unknown_variant match_other_enum_variant match_binding_type match_arm_undefined_var match_dup_default variant_ctor_arity variant_ctor_type switch_dup_enum_value switch_on_struct catch_undefined_var
         array_lit_elem_type array_lit_elem_float index_negative_literal index_by_float index_by_string alias_array_index_oob method_on_int method_on_generic_prim member_of_array forin_over_int
         const_ptr_member_write const_ptr_index_write const_self_write const_ptr_nested_field_write const_ptr_deref_method const_ptr_through_generic
         constraint_inferred_unsat constraint_inferred_prim constraint_multi_one_missing constraint_struct_param constraint_method_signature constraint_unknown_iface sizeof_generic_uninstantiated alias_unknown_target alias_generic_not_imported alias_cycle_generic
         lambda_reconciled_return nullable_non_pointer await_non_future ptr_arith_to_int string_arith_to_int ternary_void_arms
         macro_unterminated_call closure_captured_field_write method_param_self operator_arity alias_nullable_deref nullable_global_call nullable_addr_taken enum_value_cycle enum_value_cycle_generic generic_variant_arg_type generic_variant_bare struct_lit_generic_bare struct_lit_not_generic cast_from_interface global_init_const_drop global_init_type constraint_self_param scalar_brace_init ptr_conv_unrelated union_two_members global_forward_ref fn_forward_global global_shift_range generic_void_field void_slice array_size_nonconst question_float_ok switch_dup_cast_trunc closure_param_captured_escaping closure_param_thread_capture closure_param_local_lambda_escapes nullable_global_call_in_cond nullable_global_call_ternary nullable_global_call_guard nullable_global_call_branch match_on_enum_pointer match_on_struct cast_sum_to_int cast_int_to_sum struct_lit_of_enum struct_lit_of_sum call_enum_member method_as_value call_nonfn_field variadic_fn_to_int call_array_var variant_missing_args sizeof_function sizeof_variant sizeof_enum_member incdec_enum enum_cycle_alias struct_cycle_alias array_dim_later_const sizeof_later_global ptr_conv_sum_struct ptr_conv_sum_int ptr_conv_struct_sum ptr_conv_array_struct ptr_conv_slice_struct struct_lit_generic_ptr_field generic_uninit_read index_null index_null_assign deref_null variant_ctor_literal_range variant_ctor_literal_range_generic va_start_not_variadic va_start_not_va_list va_arg_not_va_list va_arg_struct thread_create_int thread_create_wrong_fn thread_join_int free_closure_int bitfield_adt_enum bitfield_zero_width method_as_field_write method_as_field_read string_slice_elem alloc_with_count_string alloc_with_no_alloc ptr_sub_mismatch
         nullable_global_operator nullable_global_operator_cond nullable_global_lambda nullable_static_call nullable_static_lambda
         alloc_with_by_value alloc_with_unknown_elem alloc_with_void_elem alloc_with_bad_ret alloc_with_bad_params
         ptr_array_from_addr ptr_array_deref
         struct_cycle_array_alias
         thread_handle_to_int
         ternary_unrelated_ptrs iface_ternary_nonconforming
         va_arg_float va_arg_narrow_int
         array_dim_cast_zero
         nullable_global_alloc_with nullable_global_thread_join nullable_global_thread_create nullable_guard_else_assign nullable_guard_then_assign array_dim_sizeof_zero not_void_call throw_void deref_void_ptr cast_void_call cast_int_to_struct_generic cast_array_literal generic_void_param closure_captured_address closure_captured_slice finally_return match_alias_nonexhaustive finally_question
         generic_deduce_conflict generic_ptr_arg_mismatch generic_struct_param_arg slice_temporary_array addr_of_rvalue_member assign_rvalue_element operator_ptr_arg_mismatch
         const_div_zero_name const_rem_zero_sizeof const_div_float_cast const_shift_sizeof const_shift_float_cast const_div_overflow const_rem_overflow_int64 float_cast_const_name_range float_cast_unsigned_negative index_oob_sizeof dangling_cast dangling_ternary struct_cycle_generic_alias assign_array_literal cast_fn_name_to_int escape_unknown escape_octal_range escape_hex_empty
         case_decl_redefined case_decl_out_of_scope
         index_oob_sizeof_struct switch_dup_sizeof_ptr const_div_zero_sizeof_expr
         enum_value_not_const enum_value_expr_range variant_nullary_parens type_nesting_too_deep
         switch_dup_sizeof_generic switch_dup_sizeof_adt switch_dup_sizeof_bitfield switch_dup_sizeof_iface
         did_you_mean_var did_you_mean_fn did_you_mean_type did_you_mean_member did_you_mean_method deprecated_not_function
         asm_output_not_lvalueasm_output_constraint asm_output_const asm_output_struct asm_output_bool asm_output_bitfield asm_output_captured asm_input_output_constraint enum_value_ternary_not_const iface_compare_ptr nullable_ptr_to_ptr_deref nullable_ptr_to_ptr_drop alias_in_composite_mismatch variant_expected_payload"
HANDLED="$(echo $HANDLED)"   # collapse the multi-line list to single spaces for matching

DRIVER=selfhost/esk_main.esk
TCBIN="$(mktemp -t tc_main.XXXXXX)"
trap 'rm -f "$TCBIN"' EXIT
if ! "$BIN" "$DRIVER" -o "$TCBIN" >/dev/null 2>/tmp/tcbuild.$$; then
    echo "tc_parity: failed to build $DRIVER"; cat /tmp/tcbuild.$$; rm -f /tmp/tcbuild.$$; exit 2
fi
rm -f /tmp/tcbuild.$$

fail=0

# ── positives: verdict must match the C++ oracle ──
pos=0
echo "Positive corpus (verdict must match --test-typechecker):"
for f in tests/*.esk; do
    [ -f "$f" ] || continue
    cref=0; "$BIN" --test-typechecker "$f" >/dev/null 2>&1 || cref=1
    cgot=0; ESKIU_ROOT="$(pwd)" "$TCBIN" --test-typechecker "$f" >/dev/null 2>&1 || cgot=1
    if [ "$cref" -eq "$cgot" ]; then
        pos=$((pos + 1))
    else
        echo "  FAIL  ${f#tests/}  (C++ verdict=$cref, self-host=$cgot)"
        fail=1
    fi
done
echo "  ok: $pos/$(ls tests/*.esk | wc -l | tr -d ' ') positive files agree"

# ── negatives: handled error classes must be rejected with the right message ──
echo "Negative corpus (handled error classes):"
for esk in tests/errors/*.esk; do
    [ -e "$esk" ] || continue
    base="$(basename "$esk" .esk)"
    # Not sema's job — caught upstream by the (already self-hosted) lexer / parser /
    # preprocessor, not the type checker.
    # These reject with the right VERDICT but the self-host lexer/parser wording still
    # differs from C++ (a diagnostic-text consistency item, not a behavioral one).
    UPSTREAM=" "
    case " $HANDLED " in
        *" $base "*) ;;
        *) case "$UPSTREAM" in
               *" $base "*) echo "  skip  errors/$base  (upstream: lexer/parser/pp)" ;;
               *) echo "  skip  errors/$base  (sema — not yet implemented)" ;;
           esac
           continue ;;
    esac
    want="$(grep -m1 'EXPECT-ERROR:' "$esk" | sed 's/.*EXPECT-ERROR:[[:space:]]*//')"
    out="$(ESKIU_ROOT="$(pwd)" "$TCBIN" --test-typechecker "$esk" 2>&1)"; code=$?
    if [ "$code" -eq 0 ]; then
        echo "  FAIL  errors/$base  (accepted code that should be rejected)"; fail=1
    elif [ -n "$want" ] && ! printf '%s' "$out" | grep -qF "$want"; then
        echo "  FAIL  errors/$base  (rejected, but message missing \"$want\")"; fail=1
    else
        # Same location as the C++ checker: the line:col of the first error that
        # carries the expected text (a leading `L:C: ` in the text is not matched), or
        # of the first error when the text is on a separate note line.
        wm="$(printf '%s' "$want" | sed -E 's/^[0-9]+:[0-9]+: //')"
        loc() {
            local o l; o="$(cat)"
            l="$(printf '%s\n' "$o" | grep -F -- "$wm" | grep -m1 '^error:')"
            [ -n "$l" ] || l="$(printf '%s\n' "$o" | grep -m1 '^error:')"
            printf '%s\n' "$l" | sed -E 's/^error: [^:]*:([0-9]+):([0-9]+):.*/\1:\2/'
        }
        ref="$("$BIN" --test-typechecker "$esk" 2>&1 | loc)"
        got="$(printf '%s\n' "$out" | loc)"
        if [ -n "$wm" ] && [ -n "$ref" ] && [ "$ref" != "$got" ]; then
            echo "  FAIL  errors/$base  (reported at ${got:-?}, the C++ checker at $ref)"; fail=1
        else
            echo "  ok    errors/$base"
        fi
    fi
done

# ── diagnostic text: the "did you mean" hints and the deprecation warnings ──
# Every line carrying a hint or a `deprecated` warning must be the same in both
# checkers, byte for byte (with and without -Wno-deprecated).
echo "Hint and deprecation text (byte-exact):"
for esk in tests/errors/did_you_mean_*.esk tests/warnings/deprecated_use.esk tests/errors/match_*nonexhaustive*.esk \
           tests/warnings/string_compare.esk tests/warnings/packed_address.esk tests/warnings/dangling_pointer.esk \
           tests/warnings/frame_larger.esk tests/warnings/format.esk tests/warnings/unused_result.esk \
           tests/warnings/await_holding_lock.esk; do
    [ -e "$esk" ] || continue
    for flags in "" "-Wno-deprecated" "-Wno-unused-result -Wno-await-holding-lock"; do
        pick() { grep -E "did you mean|warning: '.*' is deprecated|non-exhaustive match|\[-W[a-z-]+\]" || true; }
        ref="$("$BIN" --test-typechecker $flags "$esk" 2>&1 | pick)"
        got="$(ESKIU_ROOT="$(pwd)" "$TCBIN" --test-typechecker $flags "$esk" 2>&1 | pick)"
        if [ -z "$flags" ] && [ -z "$ref" ]; then
            echo "  FAIL  $esk  (the C++ checker printed no hint or warning)"; fail=1
        elif [ "$ref" != "$got" ]; then
            echo "  FAIL  $esk $flags"; echo "$ref" | sed 's/^/    C++:  /'; echo "$got" | sed 's/^/    self: /'; fail=1
        else
            echo "  ok    $esk $flags"
        fi
    done
done

# ── -Wall / -Wextra lint: every warning line, byte for byte, in the same order ──
# (the three corpus files hold signed/unsigned comparisons for -Wextra).
echo "Lint warnings under -Wall (byte-exact):"
for esk in tests/warnings/*.esk tests/sizeof_mixed_sign.esk tests/const_fold_typed_c.esk tests/bitfield_promote.esk; do
    [ -e "$esk" ] || continue
    for flags in "-Wall" "-Wall -Wextra"; do
        ref="$("$BIN" --test-typechecker $flags "$esk" 2>&1 | grep 'warning:' || true)"
        got="$(ESKIU_ROOT="$(pwd)" "$TCBIN" --test-typechecker $flags "$esk" 2>&1 | grep 'warning:' || true)"
        if [ "$ref" != "$got" ]; then
            echo "  FAIL  $esk $flags"; diff <(echo "$ref") <(echo "$got") | sed 's/^/    /'; fail=1
        else
            echo "  ok    $esk $flags"
        fi
    done
done

echo "----"
if [ "$fail" -eq 0 ]; then echo "tc parity: OK"; else echo "tc parity: MISMATCH"; fi
exit "$fail"
