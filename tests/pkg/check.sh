#!/usr/bin/env bash
#
# Package manager checks: eskiuc new / add / build / run / test / fetch, package
# imports, transitive dependencies, conflicts, lock reuse and the errors. Git
# dependencies are local repositories reached through file:// URLs (no network), and
# $ESKIU_HOME points into a temporary directory. When the self-hosted compiler is
# found (ESKIUC_ESK, else eskiuc-esk next to ESKIUC) it builds the same package and
# must report the same import errors at the same line:col.
#
# Usage: tests/pkg/check.sh     (ESKIUC / ESKIUC_ESK pick the compilers)
# Prints "ok NAME" or "FAIL NAME: why" per check, "skip ..." without git; exits 1 on
# any failure.
set -u

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
ESKIUC="${ESKIUC:-$root/build/eskiuc}"
case "$ESKIUC" in /*) ;; *) ESKIUC="$PWD/$ESKIUC" ;; esac
ESK="${ESKIUC_ESK:-$(dirname "$ESKIUC")/eskiuc-esk}"
case "$ESK" in /*) ;; *) ESK="$PWD/$ESK" ;; esac
[[ -x "$ESKIUC" ]] || { echo "FAIL packages: compiler not found at $ESKIUC"; exit 2; }
if ! command -v git > /dev/null 2>&1; then
    echo "skip packages: git not found"
    exit 0
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
export ESKIU_ROOT="$root"
export ESKIU_HOME="$work/home"
# eskiuc-esk links with $CLANG (default clang); CI names it through $CC (clang-22).
if [[ -z "${CLANG:-}" && -n "${CC:-}" ]]; then export CLANG="${CC%% *}"; fi
export GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null
export GIT_AUTHOR_NAME=eskiu GIT_AUTHOR_EMAIL=eskiu@example.com
export GIT_COMMITTER_NAME=eskiu GIT_COMMITTER_EMAIL=eskiu@example.com

fail=0
ok()  { echo "ok $1"; }
bad() { echo "FAIL $1: $2"; fail=1; }

# A git repository at $1 holding the files written by the caller, committed and
# tagged $2.
commit_repo() {
    (cd "$1" && { [[ -d .git ]] || git -c init.defaultBranch=main init -q; } &&
        git add -A && git commit -qm "commit" && { [[ -z "$2" ]] || git tag "$2"; })
}

# ---- leaf: a git package with a tag ----------------------------------------
mkdir -p "$work/src/leaf/src"
printf 'int leaf_value() { return 7; }\n' > "$work/src/leaf/src/core.esk"
commit_repo "$work/src/leaf" v1.0

# ---- mid: a git package (lib.esk) that depends on leaf ----------------------
mkdir -p "$work/src/mid"
cat > "$work/src/mid/eskiu.toml" <<EOF
[package]
name = "mid"   # a comment
version = "1.0.0"

[deps]
leaf = { git = "file://$work/src/leaf", rev = "v1.0" }
EOF
cat > "$work/src/mid/lib.esk" <<'EOF'
import <leaf/core>;
int mid_value() { return leaf_value() * 6; }
EOF
commit_repo "$work/src/mid" ""

# ---- util: a path package ---------------------------------------------------
mkdir -p "$work/util"
printf 'int util_twice(int x) { return x * 2; }\n' > "$work/util/math.esk"

# ---- new ---------------------------------------------------------------------
cd "$work" || exit 2
if "$ESKIUC" new app > /dev/null 2>&1 && [[ -f app/eskiu.toml && -f app/src/main.esk && -f app/.gitignore ]]; then
    ok "new"
else
    bad "new" "missing files"
fi
cd "$work/app" || exit 2
out="$("$ESKIUC" run 2>&1)"
if [[ "$out" == "Hello from app!" ]]; then ok "run/hello"; else bad "run/hello" "got '$out'"; fi
if (cd "$work" && "$ESKIUC" new app > /dev/null 2>&1); then bad "new/exists" "accepted an existing directory"; else ok "new/exists"; fi

# ---- add --path / add --git ---------------------------------------------------
if "$ESKIUC" add util --path ../util > /dev/null 2>&1 && grep -q '^util = { path = "../util" }$' eskiu.toml &&
        grep -q "^util	path	../util$" eskiu.lock; then
    ok "add/path"
else
    bad "add/path" "$(cat eskiu.toml eskiu.lock 2>&1)"
fi
if "$ESKIUC" add mid --git "file://$work/src/mid" > "$work/add.out" 2> "$work/add.err" &&
        grep -q "^mid = { git = \"file://$work/src/mid\" }$" eskiu.toml &&
        grep -q "^mid	git	file://$work/src/mid	HEAD	[0-9a-f]\{40\}$" eskiu.lock &&
        grep -q "^leaf	git	file://$work/src/leaf	v1.0	[0-9a-f]\{40\}$" eskiu.lock; then
    ok "add/git+transitive"
else
    bad "add/git+transitive" "$(cat "$work/add.err" eskiu.toml eskiu.lock 2>&1)"
fi

# ---- build / run / imports ----------------------------------------------------
mkdir -p src tests
cat > src/helper.esk <<'EOF'
int helper() { return 3; }
EOF
cat > src/main.esk <<'EOF'
import <io>;
import <mid>;
import <leaf/core>;
import <util/math>;
import <app/helper>;

int main(int argc, string* argv) {
    printf("%d %d %d %d %d\n", mid_value(), leaf_value(), util_twice(5), helper(), argc - 1);
    return 0;
}
EOF
if "$ESKIUC" build > "$work/build.out" 2>&1 && [[ -x build/app ]] && [[ "$(build/app)" == "42 7 10 3 0" ]]; then
    ok "build"
else
    bad "build" "$(cat "$work/build.out")"
fi
out="$("$ESKIUC" run a b 2>&1)"
if [[ "$out" == "42 7 10 3 2" ]]; then ok "run/args"; else bad "run/args" "got '$out'"; fi
# A file inside the package compiled directly uses the lock too.
if "$ESKIUC" src/main.esk -o "$work/direct" > /dev/null 2>&1 && [[ "$("$work/direct")" == "42 7 10 3 0" ]]; then
    ok "compile/file-in-package"
else
    bad "compile/file-in-package" "did not build"
fi

# ---- test ----------------------------------------------------------------------
printf 'import <mid>;\nint main() { return mid_value() == 42 ? 0 : 1; }\n' > tests/good.esk
printf 'int main() { return 3; }\n' > tests/bad.esk
out="$("$ESKIUC" test 2>&1)"; code=$?
if [[ $code -eq 1 && "$out" == *"FAIL bad (exit 3)"* && "$out" == *"PASS good"* && "$out" == *"1 passed, 1 failed"* ]]; then
    ok "test"
else
    bad "test" "exit $code: $out"
fi
rm tests/bad.esk

# ---- unknown package --------------------------------------------------------
printf 'import <io>;\nimport <nope/x>;\nint main() { return 0; }\n' > "$work/app/src/unknown.esk"
err="$("$ESKIUC" src/unknown.esk --test-typechecker 2>&1)"; code=$?
want="src/unknown.esk:2:8: unknown package 'nope' (not in eskiu.toml)"
if [[ $code -ne 0 && "$err" == *"$want"* ]]; then ok "error/unknown-package"; else bad "error/unknown-package" "$err"; fi
printf 'import <nopkg>;\nint main() { return 0; }\n' > "$work/app/src/unknown2.esk"
err="$("$ESKIUC" src/unknown2.esk -o "$work/u2" 2>&1)"
if [[ "$err" == *"src/unknown2.esk:1:8: unknown package 'nopkg' (not in eskiu.toml)"* ]]; then
    ok "error/unknown-package-bare"
else
    bad "error/unknown-package-bare" "$err"
fi

# ---- fetch: a lock pins the commit until eskiuc fetch ------------------------
old_mid="$(awk -F'\t' '$1 == "mid" { print $5 }' eskiu.lock)"
printf 'int mid_value() { return 99; }\n' > "$work/src/mid/lib.esk"
commit_repo "$work/src/mid" ""
"$ESKIUC" build > /dev/null 2>&1
if [[ "$(build/app)" == "42 7 10 3 0" ]]; then ok "lock/pins"; else bad "lock/pins" "got $(build/app)"; fi
"$ESKIUC" fetch > "$work/fetch.out" 2>&1
new_mid="$(awk -F'\t' '$1 == "mid" { print $5 }' eskiu.lock)"
"$ESKIUC" build > /dev/null 2>&1
if [[ "$old_mid" != "$new_mid" && "$(build/app)" == "99 7 10 3 0" ]]; then
    ok "fetch/updates"
else
    bad "fetch/updates" "$(cat "$work/fetch.out")"
fi

# ---- lock reuse: no git access when the lock and the checkouts match --------
mv "$work/src" "$work/src.moved"
rm -rf build
if "$ESKIUC" build > "$work/reuse.out" 2>&1 && ! grep -q fetching "$work/reuse.out" && [[ -x build/app ]]; then
    ok "lock/reuse"
else
    bad "lock/reuse" "$(cat "$work/reuse.out")"
fi
mv "$work/src.moved" "$work/src"

# ---- a new resolution of HEAD or a branch fetches first ----------------------
# A mirror left by an earlier run must not answer with the commit it had then.
mkdir -p "$work/src/solo"
printf '[package]\nname = "solo"\nversion = "0.1.0"\n' > "$work/src/solo/eskiu.toml"
printf 'int solo_value() { return 1; }\n' > "$work/src/solo/solo.esk"
commit_repo "$work/src/solo" ""
for project in first second; do
    mkdir -p "$work/$project/src"
    printf '[package]\nname = "%s"\nversion = "0.1.0"\nmain = "src/main.esk"\n\n[deps]\nsolo = { git = "file://%s/src/solo" }\n' \
        "$project" "$work" > "$work/$project/eskiu.toml"
    printf 'import <solo>;\nint main() { return solo_value(); }\n' > "$work/$project/src/main.esk"
    (cd "$work/$project" && "$ESKIUC" build > /dev/null 2>&1)
    if [[ $project == first ]]; then
        printf 'int solo_value() { return 2; }\n' > "$work/src/solo/solo.esk"
        commit_repo "$work/src/solo" ""
    fi
done
head_solo="$(git -C "$work/src/solo" rev-parse HEAD)"
got_solo="$(awk -F'\t' '$1 == "solo" { print $5 }' "$work/second/eskiu.lock")"
if [[ "$got_solo" == "$head_solo" ]]; then ok "resolve/head-fetches"; else bad "resolve/head-fetches" "got $got_solo, want $head_solo"; fi

# ---- errors ------------------------------------------------------------------
cp eskiu.toml "$work/toml.bak"
err="$("$ESKIUC" add bogus --git "file://$work/src/leaf" --rev nosuch 2>&1)"; code=$?
if [[ $code -ne 0 && "$err" == *"rev 'nosuch' not found in file://$work/src/leaf (package 'bogus')"* ]] &&
        cmp -s eskiu.toml "$work/toml.bak"; then
    ok "error/rev-not-found"
else
    bad "error/rev-not-found" "$err"
fi
err="$("$ESKIUC" add gone --git "file://$work/missing-repo" 2>&1)"; code=$?
if [[ $code -ne 0 && "$err" == *"could not clone file://$work/missing-repo (package 'gone')"* ]]; then
    ok "error/clone"
else
    bad "error/clone" "$err"
fi
# Two requirers of one package at different commits.
"$ESKIUC" add leaf --git "file://$work/src/leaf" --rev main > /dev/null 2>&1
(cd "$work/src/leaf" && printf '// v2\n' >> src/core.esk && git commit -qam v2)
"$ESKIUC" add leaf --git "file://$work/src/leaf" --rev main > /dev/null 2>&1
err="$("$ESKIUC" fetch 2>&1)"; code=$?
if [[ $code -ne 0 && "$err" == *"package 'leaf' is required twice with different sources: commit "*" by 'app' and commit "*" by 'mid'"* ]]; then
    ok "error/conflict"
else
    bad "error/conflict" "$err"
fi
cp "$work/toml.bak" eskiu.toml
"$ESKIUC" fetch > /dev/null 2>&1
printf '[package]\nname = "app"\n\n[deps]\nx = { git = "u", rev = }\n' > eskiu.toml
err="$("$ESKIUC" build 2>&1)"; code=$?
if [[ $code -ne 0 && "$err" == "error: eskiu.toml:5: expected a string value for 'rev'" ]]; then
    ok "error/bad-toml"
else
    bad "error/bad-toml" "$err"
fi
cp "$work/toml.bak" eskiu.toml
mkdir -p "$work/nogit/bin"
ln -s "$ESKIUC" "$work/nogit/bin/eskiuc"
rm -rf "$ESKIU_HOME"
# A Windows eskiuc.exe loads DLLs (libLLVM) from PATH: keep its directory and MinGW's.
nogit_path="$work/nogit/bin"
case "$ESKIUC" in *.exe) nogit_path="$nogit_path:$(dirname "$ESKIUC"):${MINGW_PREFIX:-/mingw64}/bin" ;; esac
if PATH="$nogit_path" command -v git > /dev/null 2>&1; then
    echo "skip error/no-git (git is next to the compiler's DLLs)"
else
    err="$(PATH="$nogit_path" eskiuc build 2>&1)"; code=$?
    if [[ $code -ne 0 && "$err" == *"git not found on PATH (needed to fetch package '"* ]]; then
        ok "error/no-git"
    else
        bad "error/no-git" "$err"
    fi
fi
"$ESKIUC" build > /dev/null 2>&1

# ---- outside a package, `eskiuc run file.esk` is unchanged -------------------
printf 'int main() { return 5; }\n' > "$work/plain.esk"
(cd "$work" && "$ESKIUC" run plain.esk > /dev/null 2>&1); code=$?
if [[ $code -eq 5 ]]; then ok "run/script-outside"; else bad "run/script-outside" "exit $code"; fi
(cd "$work/app" && "$ESKIUC" run "$work/plain.esk" > /dev/null 2>&1); code=$?
if [[ $code -eq 5 ]]; then ok "run/script-inside"; else bad "run/script-inside" "exit $code"; fi

# ---- the self-hosted compiler resolves the same imports ---------------------
if [[ -x "$ESK" ]]; then
    if "$ESK" src/main.esk -o "$work/esk_app" > "$work/esk.out" 2>&1 &&
            [[ "$("$work/esk_app" x)" == "99 7 10 3 1" ]]; then
        ok "selfhost/build"
    else
        bad "selfhost/build" "$(head -5 "$work/esk.out")"
    fi
    for f in unknown unknown2; do
        a="$("$ESKIUC" "src/$f.esk" --test-typechecker 2>&1 | grep '^error:')"
        b="$("$ESK" "src/$f.esk" --test-typechecker 2>&1 | grep '^error:')"
        if [[ -n "$a" && "$a" == "$b" ]]; then ok "selfhost/$f"; else bad "selfhost/$f" "'$a' vs '$b'"; fi
    done
else
    echo "skip selfhost packages: $ESK not found"
fi

# ---- a native library from a prebuilt archive ([native.NAME]) ---------------
# A C library in a .tgz reached through file://, declared for this host's platform:
# the build that links it fetches and checks it, the program links and runs with no -L, a
# second build reuses the cache, a direct compile in the package fetches it too, a program
# that does not link it fetches nothing, and a wrong sha256 or an unknown platform is
# refused.
os="$(uname -s)"; arch="$(uname -m)"
case "$os" in Darwin) plat=macos ;; Linux) plat=linux ;; MINGW*|MSYS*) plat=windows ;; *) plat="" ;; esac
case "$arch" in arm64|aarch64) plat="$plat-arm64" ;; x86_64|amd64) plat="$plat-x86_64" ;; *) plat="" ;; esac
cc_prog="${CC:-cc}"; cc_prog="${cc_prog%% *}"
if [[ -n "$plat" && "$plat" != -* ]] && command -v "$cc_prog" > /dev/null 2>&1 && command -v curl > /dev/null 2>&1; then
    mkdir -p "$work/natsrc/natlib/lib" "$work/nat/src"
    printf 'int nat_value(void) { return 41; }\n' > "$work/natsrc/nat.c"
    case "$plat" in
        macos-*)   "$cc_prog" -shared -o "$work/natsrc/natlib/lib/libnat.dylib" -install_name @rpath/libnat.dylib "$work/natsrc/nat.c" ;;
        windows-*) "$cc_prog" -shared -o "$work/natsrc/natlib/lib/nat.dll" "$work/natsrc/nat.c" ;;
        *)         "$cc_prog" -shared -fPIC -o "$work/natsrc/natlib/lib/libnat.so" "$work/natsrc/nat.c" ;;
    esac
    (cd "$work/natsrc" && tar -czf natlib.tgz natlib)
    if command -v sha256sum > /dev/null 2>&1; then sum="$(sha256sum "$work/natsrc/natlib.tgz" | cut -d' ' -f1)"
    else sum="$(shasum -a 256 "$work/natsrc/natlib.tgz" | cut -d' ' -f1)"; fi
    natfile="$work/natsrc/natlib.tgz"
    if command -v cygpath > /dev/null 2>&1; then natfile="/$(cygpath -m "$natfile")"; fi
    printf '[package]\nname = "nat"\n\n[native.nat]\n%s = { url = "file://%s", sha256 = "%s", lib = "natlib/lib" }\n' \
        "$plat" "$natfile" "$sum" > "$work/nat/eskiu.toml"
    printf 'import <io>;\n#pragma link("nat")\nextern int nat_value();\nint main() { printf("%%d\\n", nat_value()); return 0; }\n' \
        > "$work/nat/src/main.esk"
    out="$(cd "$work/nat" && "$ESKIUC" run 2>&1)"
    if [[ "$out" == *"fetching nat for nat from file://"* && "$out" == *"41" ]]; then ok "native/fetch-link-run"; else bad "native/fetch-link-run" "$out"; fi
    mv "$work/natsrc/natlib.tgz" "$work/natsrc/natlib.tgz.away"
    out="$(cd "$work/nat" && "$ESKIUC" run 2>&1)"
    if [[ "$out" == "41" ]]; then ok "native/cache"; else bad "native/cache" "$out"; fi
    out="$(cd "$work/nat" && "$ESKIUC" src/main.esk -o "$work/nat_direct" 2>&1 && "$work/nat_direct")"
    if [[ "$out" == "41" ]]; then ok "native/direct-compile"; else bad "native/direct-compile" "$out"; fi
    mv "$work/natsrc/natlib.tgz.away" "$work/natsrc/natlib.tgz"
    rm -rf "$ESKIU_HOME/native"
    printf 'int main() { return 3; }\n' > "$work/nat/src/other.esk"
    (cd "$work/nat" && "$ESKIUC" src/other.esk -o "$work/nat_other" > /dev/null 2>&1)
    if [[ ! -d "$ESKIU_HOME/native" ]]; then ok "native/unused-not-fetched"; else bad "native/unused-not-fetched" "fetched"; fi
    out="$(cd "$work/nat" && "$ESKIUC" src/main.esk -o "$work/nat_direct" 2>&1 && "$work/nat_direct")"
    if [[ "$out" == *"fetching nat for nat from file://"* && "$out" == *"41" ]]; then
        ok "native/direct-fetch"
    else
        bad "native/direct-fetch" "$out"
    fi
    zeros="0000000000000000000000000000000000000000000000000000000000000000"
    sed "s/$sum/$zeros/" "$work/nat/eskiu.toml" > "$work/nat/toml.new" && mv "$work/nat/toml.new" "$work/nat/eskiu.toml"
    out="$(cd "$work/nat" && "$ESKIUC" build 2>&1)"; code=$?
    if [[ $code -ne 0 && "$out" == *"has sha256 $sum, not $zeros"* ]]; then ok "native/bad-sha256"; else bad "native/bad-sha256" "$out"; fi
    printf '[package]\nname = "nat"\n\n[native.nat]\nmacos = { url = "x", sha256 = "y" }\n' > "$work/nat/eskiu.toml"
    out="$(cd "$work/nat" && "$ESKIUC" build 2>&1)"; code=$?
    if [[ $code -ne 0 && "$out" == "error: eskiu.toml:5: unknown platform 'macos' in [native.nat] (macos-arm64, macos-x86_64, linux-x86_64, linux-arm64, windows-x86_64, windows-arm64)" ]]; then
        ok "native/bad-platform"
    else
        bad "native/bad-platform" "$out"
    fi
else
    echo "skip native libraries (no platform key, C compiler or curl)"
fi

# ---- a path dependency is relative in the lock: the tree can move -----------
mkdir -p "$work/moved" && cp -R "$work/util" "$work/moved/util" && mkdir -p "$work/moved/app/src"
printf 'import <io>;\nimport <util/math>;\nint main() { printf("%%d\\n", util_twice(21)); return 0; }\n' > "$work/moved/app/src/main.esk"
printf '[package]\nname = "app"\nversion = "0.1.0"\n\n[deps]\nutil = { path = "../util" }\n' > "$work/moved/app/eskiu.toml"
(cd "$work/moved/app" && "$ESKIUC" build > /dev/null 2>&1)
mv "$work/moved" "$work/moved2"
if [[ "$(cd "$work/moved2/app" && "$ESKIUC" run 2>&1)" == "42" ]]; then
    ok "lock/relative-path"
else
    bad "lock/relative-path" "$(cd "$work/moved2/app" && "$ESKIUC" run 2>&1 | head -3)"
fi

exit $fail
