#!/usr/bin/env bash
#
# The 0.10 scripting stdlib tests on native Windows (MSYS2 MINGW64), run by
# .github/workflows/windows.yml. Each test is built with eskiuc.exe and run with
# stdout and stderr together, as tests/run.sh runs it. A test with a .expected
# file must print that text (CRs are dropped on both sides: the checkout may
# have CRLF line ends), one without must exit 0. tests/process_run.esk drives
# /bin/sh, so tests/process_self.esk covers <process> here.
#
# Usage: ESKIUC=build/eskiuc.exe tests/windows/run.sh
set -u

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
ESKIUC="${ESKIUC:-$root/build/eskiuc.exe}"
export ESKIU_ROOT="$root"
work="$root/winbin"
mkdir -p "$work"

tests=(string_script list_script map_script json_build csv dotenv hash url_parse
       args term log time_format console_io fs_dir fs_tree_fd process_self dns_connect fetch_dns
       fetch_basic fetch_raw fetch_async fetch_no_tls fetch_https
       executor executor_many executor_cross_thread)

fail=0
failed=()
for t in "${tests[@]}"; do
    src="$root/tests/$t.esk"
    if ! "$ESKIUC" "$src" -o "$work/$t.exe" > "$work/$t.build" 2>&1; then
        echo "FAIL $t (build)"; sed 's/^/    /' "$work/$t.build" | head -20
        fail=1; failed+=("$t"); continue
    fi
    timeout 120 "$work/$t.exe" > "$work/$t.raw" 2>&1
    code=$?
    tr -d '\r' < "$work/$t.raw" > "$work/$t.out"
    exp="$root/tests/$t.expected"
    if [[ -f "$exp" && $code -eq 0 ]]; then
        tr -d '\r' < "$exp" > "$work/$t.want"
        if diff -u "$work/$t.want" "$work/$t.out" > "$work/$t.diff"; then
            echo "ok   $t"
        else
            echo "FAIL $t (output)"; sed 's/^/    /' "$work/$t.diff" | head -80
            fail=1; failed+=("$t")
        fi
    elif [[ $code -eq 0 ]]; then
        echo "ok   $t (smoke)"
    else
        echo "FAIL $t (exit $code)"; tail -40 "$work/$t.out" | sed 's/^/    /'
        fail=1; failed+=("$t")
    fi
done
if [[ $fail -ne 0 ]]; then echo "failed: ${failed[*]}"; fi
exit "$fail"
