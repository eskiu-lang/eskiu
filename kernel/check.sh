#!/bin/sh
# check.sh: boot the kernel in QEMU headless, type a scripted shell session on its
# serial input and check the UART output.
# Run through `make check`, which passes QEMU, QEMU_FLAGS and TIMEOUT.
# The session ends with `poweroff` (PSCI SYSTEM_OFF), so QEMU exits by itself; the
# timeout only catches a hang.

QEMU=${QEMU:-qemu-system-aarch64}
QEMU_FLAGS=${QEMU_FLAGS:-"-M virt,gic-version=2 -cpu cortex-a57 -nographic -kernel kernel-O0.elf"}
TIMEOUT=${TIMEOUT:-30}

out=$(mktemp)
in=$(mktemp)
trap 'rm -f "$out" "$in"' EXIT

# \177 is DEL, what the Backspace key sends: "echx<DEL>o fixed" is "echo fixed".
printf 'help\nmem\nuptime\nregs\ntasks\necho hello, kernel\nechx\177o fixed\nfault\nbogus\npoweroff\n' > "$in"

# A watchdog instead of timeout(1), which macOS does not ship.
# shellcheck disable=SC2086
$QEMU $QEMU_FLAGS < "$in" > "$out" 2>&1 &
qpid=$!
( sleep "$TIMEOUT"; kill "$qpid" 2> /dev/null ) &
wpid=$!
wait "$qpid"
status=$?
kill "$wpid" 2> /dev/null
wait "$wpid" 2> /dev/null

tr -d '\r' < "$out" > "$out.txt" && mv "$out.txt" "$out"
cat "$out"

fail=0
if [ "$status" -ne 0 ]; then
    echo "check: QEMU exited with status $status (timed out after ${TIMEOUT}s?)"
    fail=1
fi

# One extended regex per line; each must match a whole output line.
expected='Eskiu bare-metal kernel
Running at: EL1
Timer:      [0-9]+ Hz
Exception:  breakpoint \(EC 0x3C, IL 1, ISS 0x2A\)
  ELR_EL1:  0x[0-9A-F]{16}
  Recovered: resuming at the next instruction\.
Back from the breakpoint\.
Tick:       100 Hz on IRQ 27
10 ticks:   [0-9]+ ms
UART base:  0x0000000009000000
Heap:       0x0000000040300000 - 0x0000000040400000
alloc\(64\): 0x0000000040300010
alloc\(4096\): 0x0000000040300010
alloc\(4194304\): out of memory
Heap used:  0 bytes
Boot time:  [0-9]+ us
ping: round 1 of 3
pong: round 1 of 3
ping: round 3 of 3
pong: round 3 of 3
Preempted:  main is back after [0-9]+ tick\(s\), spin is ready
eskiu> help
  poweroff    switch the machine off
eskiu> mem
Heap:       [0-9]+ bytes used, [0-9]+ bytes free
eskiu> uptime
Uptime:     [0-9]+\.[0-9]{2} s \([0-9]+ ticks\)
eskiu> regs
CurrentEL:  1
CNTFRQ_EL0: [0-9]+ Hz
VBAR_EL1:   0x00000000400[0-9A-F]{3}00
eskiu> tasks
 0  main  running  [0-9]+
 1  ping  done     [0-9]+
 2  pong  done     [0-9]+
 3  spin  (ready|done) +[0-9]+
spin has counted to [0-9]+
eskiu> echo hello, kernel
hello, kernel
fixed
eskiu> fault
Reading 0x000000000C000000
Exception:  data abort \(EC 0x25, IL 1, ISS 0x10\)
  FAR_EL1:  0x000000000C000000
Survived the fault\.
eskiu> bogus
Unknown command: bogus \(try help\)
eskiu> poweroff
Powering off\.'

echo "$expected" | while IFS= read -r pat; do
    if ! grep -Eqx -- "$pat" "$out"; then
        echo "check: missing line matching: $pat"
        exit 1
    fi
done || fail=1

if [ "$fail" -ne 0 ]; then
    echo "check: FAILED"
    exit 1
fi
echo "check: OK"
