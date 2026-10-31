#!/usr/bin/env bash
# Linux smoke of a release, run from a Mac (or any Docker host): a pre-release check,
# not a CI gate.
#
# The compiler cross-compiles for Linux on the host (no Linux build of eskiuc needed);
# the objects are linked with gcc (C companions built with clang) and run inside a Linux container:
#   1. every run/smoke test in tests/*.esk (or the ones named in $TESTS): a run test
#      must exit 0 and print exactly its .expected, a smoke test must exit 0;
#   2. the self-hosted drivers (selfhost/esk_main.esk, selfhost/cg_main.esk) type-check
#      (--test-typechecker, tests/*.esk and tests/errors/*.esk) and generate code for
#      (--test-codegen, tests/*.esk) the corpus without crashing (a signal or a timeout;
#      an error test is expected to exit 1);
#   3. the self-hosted code generator compiles itself on Linux: cg_main emits IR for
#      cg_main.esk, the host clang assembles it, and the result emits the same IR again.
#
# Usage: tests/linux_docker.sh
#   ESKIUC   the compiler (default build/eskiuc)
#   CLANG    a clang with the target's backend, to assemble the self-emitted IR
#            (default clang; on macOS use $(brew --prefix llvm@22)/bin/clang)
#   TRIPLE   the target (default aarch64-unknown-linux-gnu; x86_64-unknown-linux-gnu
#            runs under emulation on Apple silicon)
#   IMAGE    the base image (default ubuntu:24.04); gcc is added once, in a cached image
#   TESTS    space-separated test names, to run only those
#   JOBS     parallel jobs inside the container (default: its CPU count)
# Exits non-zero if anything failed.

set -u
cd "$(dirname "$0")/.." || exit 2
ROOT="$(pwd)"
ESKIUC="${ESKIUC:-$ROOT/build/eskiuc}"
CLANG="${CLANG:-clang}"
TRIPLE="${TRIPLE:-aarch64-unknown-linux-gnu}"
IMAGE="${IMAGE:-ubuntu:24.04}"
case "$TRIPLE" in
    aarch64*|arm64*) PLATFORM=linux/arm64 ;;
    x86_64*|amd64*)  PLATFORM=linux/amd64 ;;
    *) echo "linux_docker: unsupported TRIPLE $TRIPLE"; exit 2 ;;
esac

[[ -x "$ESKIUC" ]] || { echo "linux_docker: compiler not found at $ESKIUC (set ESKIUC)"; exit 2; }
command -v "$CLANG" >/dev/null 2>&1 || { echo "linux_docker: $CLANG not found (set CLANG)"; exit 2; }
docker info >/dev/null 2>&1 || { echo "linux_docker: docker is not available"; exit 2; }

# The base image plus gcc (the linker) and clang (the C companions), built once and reused
# (the tag names the base and platform).
SMOKE_IMAGE="eskiu-linux-smoke-clang:$(echo "$IMAGE-$PLATFORM" | tr '/:' '--')"
if ! docker image inspect "$SMOKE_IMAGE" >/dev/null 2>&1; then
    echo "Building $SMOKE_IMAGE (once)..."
    printf 'FROM %s\nRUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends gcc g++ clang libc6-dev && rm -rf /var/lib/apt/lists/*\n' "$IMAGE" \
        | docker build --platform "$PLATFORM" -q -t "$SMOKE_IMAGE" - >/dev/null \
        || { echo "linux_docker: could not build $SMOKE_IMAGE"; exit 2; }
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/obj"
fail=0

# ---- 1. cross-compile ---------------------------------------------------------
if [[ -n "${TESTS:-}" ]]; then
    names="$TESTS"
else
    source tests/resolver.sh
    names="$(for f in tests/*.esk; do n="$(basename "$f" .esk)"; resolver_skip "$n" || echo "$n"; done)"
fi
ntests=0
echo "Cross-compiling for $TRIPLE..."
for name in $names; do
    [[ -f "tests/$name.esk" ]] || { echo "  FAIL  $name (no tests/$name.esk)"; fail=1; continue; }
    if ! "$ESKIUC" --target "$TRIPLE" "tests/$name.esk" -c -o "$WORK/obj/$name.o" >"$WORK/cerr" 2>&1; then
        echo "  FAIL  $name (cross-compile: $(head -1 "$WORK/cerr"))"; fail=1; continue
    fi
    echo "$name" >> "$WORK/tests.txt"
    ntests=$((ntests + 1))
done
for drv in esk_main cg_main; do
    if ! "$ESKIUC" --target "$TRIPLE" "selfhost/$drv.esk" -c -o "$WORK/obj/$drv.o" >"$WORK/cerr" 2>&1; then
        echo "  FAIL  selfhost/$drv.esk (cross-compile: $(head -1 "$WORK/cerr"))"; exit 1
    fi
done

# ---- 2. link and run in the container -------------------------------------------
cat > "$WORK/inside.sh" <<'EOF'
#!/bin/bash
# Runs in the container: /src is the repository (read-only), /work the scratch dir.
set -u
cd /src
export ESKIU_ROOT=/src
JOBS="${JOBS:-$(nproc)}"
LIBS="-lpthread -lm -lstdc++"
mkdir -p /work/bin /work/out

run_one() {
    name="$1"
    extra=""
    if [[ -f "/src/tests/$name.c" ]]; then
        # clang, as in CI: Eskiu follows clang's C ABI where gcc differs (GCC 12 changed how
        # x86-64 passes a struct with a zero-width bitfield).
        clang -c "/src/tests/$name.c" -o "/work/obj/$name.c.o" 2>/work/out/$name.cc || { echo "FAIL  $name (C companion)"; return; }
        extra="/work/obj/$name.c.o"
    fi
    gcc "/work/obj/$name.o" $extra -o "/work/bin/$name" $LIBS 2>/work/out/$name.ld \
        || { echo "FAIL  $name (link: $(head -1 /work/out/$name.ld))"; return; }
    timeout 60 "/work/bin/$name" > "/work/out/$name.out" 2>&1; code=$?
    if [[ -f "/src/tests/$name.expected" ]]; then
        if [[ $code -ne 0 ]]; then echo "FAIL  $name (exited $code)"
        elif ! cmp -s "/src/tests/$name.expected" "/work/out/$name.out"; then echo "FAIL  $name (output differs)"
        else echo "PASS  $name"; fi
    elif [[ $code -ne 0 ]]; then echo "FAIL  $name (smoke test exited $code)"
    else echo "PASS  $name (smoke)"; fi
}
export -f run_one
export LIBS

# The self-hosted drivers, and the corpus through them (a crash is a signal or a timeout).
gcc /work/obj/esk_main.o -o /work/bin/esk_main $LIBS && gcc /work/obj/cg_main.o -o /work/bin/cg_main $LIBS \
    || { echo "FAIL  self-host drivers (link)"; exit 1; }
tc_one() {
    timeout 120 /work/bin/esk_main "$2" "$1" >/dev/null 2>&1; code=$?
    if [[ $code -ge 124 ]]; then echo "FAIL  esk_main $2 $1 (exit $code)"; else echo "PASS"; fi
}
export -f tc_one

if [[ -s /work/tests.txt ]]; then
    xargs -P "$JOBS" -I{} bash -c 'run_one {}' < /work/tests.txt > /work/run.log
fi
{ for f in tests/*.esk tests/errors/*.esk; do echo "$f --test-typechecker"; done
  for f in tests/*.esk; do echo "$f --test-codegen"; done; } > /work/corpus.txt
xargs -P "$JOBS" -L1 bash -c 'tc_one "$0" "$1"' < /work/corpus.txt > /work/corpus.log

# The self-hosted code generator on its own source.
timeout 300 /work/bin/cg_main /src/selfhost/cg_main.esk > /work/cg_self.ll 2>/work/cg_self.err
echo "$?" > /work/cg_self.code
EOF

cat > "$WORK/inside2.sh" <<'EOF'
#!/bin/bash
# Second container pass: the self-built code generator must emit the same IR again.
set -u
cd /src
export ESKIU_ROOT=/src
gcc /work/cg_self.o -o /work/bin/cg_self -lpthread -lm || { echo "link failed"; exit 1; }
timeout 300 /work/bin/cg_self /src/selfhost/cg_main.esk > /work/cg_self2.ll 2>/work/cg_self2.err || { echo "cg_self exited $?"; exit 1; }
cmp -s /work/cg_self.ll /work/cg_self2.ll || { echo "IR differs"; exit 1; }
EOF

echo "Linking and running in $IMAGE ($PLATFORM)..."
docker run --rm --platform "$PLATFORM" -e "JOBS=${JOBS:-}" -v "$ROOT":/src:ro -v "$WORK":/work \
    "$SMOKE_IMAGE" bash /work/inside.sh
[[ -f "$WORK/run.log" ]] || touch "$WORK/run.log"

# Known failures on a target (CHANGELOG "Known issues"): reported as XFAIL, and as
# XPASS once they pass, so the list can shrink; neither fails the run.
case "$TRIPLE" in
    *) KNOWN_FAIL="" ;;
esac
known() { [[ " $KNOWN_FAIL " == *" $1 "* ]]; }
run_pass=0; run_fail=0; xfail=0
while read -r verdict name rest; do
    if [[ "$verdict" == PASS ]]; then
        run_pass=$((run_pass + 1))
        known "$name" && echo "  XPASS $name (listed as a known failure on $TRIPLE)"
    elif known "$name"; then
        xfail=$((xfail + 1)); echo "  XFAIL $name $rest"
    else
        run_fail=$((run_fail + 1)); echo "  FAIL  $name $rest"
    fi
done < <(sort "$WORK/run.log")
echo "Run and smoke tests: $run_pass passed, $run_fail failed, $xfail known failures (of $ntests compiled)"
[[ $run_fail -eq 0 && $((run_pass + xfail)) -eq $ntests ]] || fail=1

corpus_pass=$(grep -c '^PASS' "$WORK/corpus.log" 2>/dev/null)
corpus_fail=$(grep -c '^FAIL' "$WORK/corpus.log" 2>/dev/null)
grep '^FAIL' "$WORK/corpus.log" 2>/dev/null | sort | sed 's/^/  /'
echo "Self-host corpus (--test-typechecker, --test-codegen): $corpus_pass ok, $corpus_fail crashed"
[[ ${corpus_fail:-1} -eq 0 && ${corpus_pass:-0} -gt 0 ]] || fail=1

# ---- 3. self-host fixpoint on Linux -----------------------------------------------
if [[ "$(cat "$WORK/cg_self.code" 2>/dev/null)" != "0" ]]; then
    echo "Self-host: cg_main could not compile cg_main.esk on Linux"; head -5 "$WORK/cg_self.err" 2>/dev/null; fail=1
elif ! "$CLANG" --target="$TRIPLE" -c "$WORK/cg_self.ll" -o "$WORK/cg_self.o" 2>"$WORK/cg_self.clang"; then
    echo "Self-host: clang rejected the IR cg_main emitted for itself"; head -5 "$WORK/cg_self.clang"; fail=1
elif ! out="$(docker run --rm --platform "$PLATFORM" -v "$ROOT":/src:ro -v "$WORK":/work "$SMOKE_IMAGE" bash /work/inside2.sh)"; then
    echo "Self-host: the self-built cg_main failed: $out"; fail=1
else
    echo "Self-host: cg_main compiles itself on Linux, and the result emits identical IR"
fi

if [[ $fail -eq 0 ]]; then echo "linux_docker: all passed ($TRIPLE)"; else echo "linux_docker: FAILED ($TRIPLE)"; fi
exit $fail
