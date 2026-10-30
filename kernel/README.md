# Eskiu Kernel

Bare-metal ARM64 kernel targeting QEMU `-M virt`. It boots without libc, handles CPU
exceptions and timer interrupts, runs a few tasks under a round-robin scheduler, and
ends in a small shell on the PL011 serial console. `poweroff` switches the machine
off.

It was the first milestone for the language, Eskiu code running on bare metal, and it is
kept building with every release.

## What it shows

- `volatile` MMIO: the UART driver reads the flag register through a bitfield struct
  (`UartFlags`), waits for TXFF to clear before each write and for RXFE to clear
  before each read. The GIC driver uses the same style.
- Exceptions: `vectors.s` saves the registers as a `TrapFrame` and calls Eskiu. The
  synchronous handler reads `ESR_EL1` into a bitfield struct (`Esr`: EC, IL, ISS),
  dispatches on the exception class with `match` over an enum, prints `ELR_EL1` (and
  `FAR_EL1` for aborts), and recovers by moving `elr` past the faulting instruction.
  The boot recovers from a `brk`; the shell's `fault` command recovers from a data
  abort. Anything else halts the machine.
- Interrupts: GICv2 setup, the EL1 virtual timer (interrupt 27) ticking at 100 Hz from
  a compare value that advances one interval per tick, and IRQs unmasked through DAIF.
- Tasks: each task has its own stack, and its saved context is the `TrapFrame` on it.
  `yield()` is `svc #0`; the handler stores the frame and returns the next task's,
  so the exception return is the context switch. Tasks are closures (`fn()->void`),
  passed as an `escaping` parameter so their environment lives on the heap. After a
  cooperative demo the timer also preempts: a task that never yields still gives the
  CPU back on the next tick.
- A shell: line editing (echo, Backspace), a sum type `Command` produced by the parser
  and run with `match`.
- `extern` globals: the linker-script symbols `__bss_start`/`__bss_end` and
  `__heap_start`/`__heap_end`, and the vector table `exception_vectors`.
- `--freestanding`: `alloc<T>`/`free` from `<mem>` call `esk_alloc`/`esk_free`, which
  wrap the stdlib `FirstFit` allocator from `<alloc>` over the linker-defined heap, so
  `free` (and `defer free`) gives the block back for reuse.
- Inline-asm output and input operands for the system registers, and PSCI
  `SYSTEM_OFF` through `hvc #0`, so QEMU exits by itself.

## Files

```
boot.s       ARM64 entry point: sets up the stack, calls kernel_main
vectors.s    exception vector table, register save/restore, eret
kernel.esk   kernel_main: clears .bss, runs the boot demos, starts the shell
uart.esk     PL011 UART driver (volatile MMIO)
cpu.esk      system registers, IRQ masking, PSCI power off (inline asm)
alloc.esk    esk_alloc / esk_free over a stdlib FirstFit allocator
trap.esk     exception handlers: ESR decoding, recovery, IRQ dispatch
gic.esk      GICv2 distributor and CPU interface
timer.esk    the 100 Hz tick from the virtual timer
sched.esk    tasks, yield, round-robin scheduling
workers.esk  the demo tasks
shell.esk    the UART shell
linker.ld    memory layout and the symbols the kernel reads
check.sh     boots the kernel headless, types a shell session, checks the output
```

`.bss` is cleared by `zero_bss()` at the top of `kernel_main` rather than in `boot.s`.
Nothing before that point reads a global, so the loop can live in Eskiu.

## Prerequisites

A built `eskiuc` (the Makefile uses `../build/eskiuc`), clang (to assemble the `.s`
files), `ld.lld`, and QEMU. On macOS:

```bash
brew install lld qemu     # or: make setup
```

On Debian or Ubuntu: `apt-get install lld qemu-system-arm`.

Every tool is a Makefile variable you can override:

```bash
make check ESKIUC=../build22/eskiuc CLANG=clang-22 LLD=ld.lld-22 QEMU=qemu-system-aarch64
```

## Build and run

```bash
cd kernel
make          # compiles and links kernel-O0.elf
make run      # boots it in QEMU on this terminal
make check    # boots an -O0 and an -O2 build headless and checks the output
```

`make run` boots to the shell. Type `help` for the commands, `poweroff` to switch
the machine off, or press `Ctrl-A X` to leave QEMU:

```
Running at: EL1
Timer:      62500000 Hz
Testing a breakpoint (brk #42):
Exception:  breakpoint (EC 0x3C, IL 1, ISS 0x2A)
  ELR_EL1:  0x0000000040002CA8
  Recovered: resuming at the next instruction.
Back from the breakpoint.

Tick:       100 Hz on IRQ 27
10 ticks:   102 ms
...
ping: round 1 of 3
pong: round 1 of 3
...
Preempted:  main is back after 1 tick(s), spin is ready

Type `help` for the commands.
eskiu> tasks
ID  NAME  STATE    SWITCHES
 0  main  running  5
 1  ping  done     4
 2  pong  done     4
 3  spin  ready    1
spin has counted to 1109443
eskiu> fault
Reading 0x000000000C000000
Exception:  data abort (EC 0x25, IL 1, ISS 0x10)
  ELR_EL1:  0x00000000400040E0
  FAR_EL1:  0x000000000C000000
  Recovered: resuming at the next instruction.
Survived the fault.
eskiu> poweroff
Powering off.
```

The shell commands are `help`, `mem` (heap used and free), `uptime` (from the tick
count), `regs` (CurrentEL, the timer frequency and counter, VBAR_EL1), `echo TEXT`,
`tasks`, `fault` and `poweroff`. The shell runs in task 0 (`main`, which is
`kernel_main` on the boot stack); while it waits for a key the core sleeps in `wfi`,
and the timer hands the CPU to `spin` until that task finishes after 3 seconds.

`make check` pipes a fixed session (`help`, `mem`, `uptime`, `regs`, `tasks`, two
`echo` lines, one with a Backspace, `fault`, an unknown command and `poweroff`) into
QEMU's serial input, kills QEMU after `TIMEOUT` seconds (default 30) in case the
kernel hangs, and fails unless QEMU exited cleanly and every expected line (listed in
`check.sh`) is present. Values that vary, such as the timer frequency or the tick
counts, only have to be numbers. CI runs it on Linux.

`make` builds at `-O0`; `make OPT=2` builds at `-O2`, and `make check` boots both.
The kernel is compiled with `--mattr=-fp-armv8,-neon` so that no FP/SIMD register is
used: the exception entry code then only has to save the general registers, and
FP/SIMD stays disabled in `CPACR_EL1`. A `float` or `double` in the kernel does not
reach the hardware: LLVM lowers it to soft-float calls (`__muldf3`, `__floatsidf`, ...)
that nothing defines here, so `ld.lld` fails with an undefined symbol.

## Memory layout

| Region          | Address              | Size  |
|-----------------|----------------------|-------|
| Kernel code     | `0x40000000`         | (n/a) |
| Boot stack      | `0x40200000`         | 64 KB |
| Heap            | `0x40300000`         | 1 MB  |
| GIC distributor | `0x08000000`         | MMIO  |
| GIC CPU iface   | `0x08010000`         | MMIO  |
| UART (PL011)    | `0x09000000`         | MMIO  |

The boot stack belongs to task 0. Every other task gets a 16 KB stack from the heap.
Exceptions run on the stack of the task they interrupt (EL1 with `SP_EL1`), so the
saved frame stays with its task.

## How it is compiled

```bash
# kernel.esk imports the other .esk files; all compile into one ELF arm64 object
eskiuc kernel.esk --target aarch64-unknown-none-elf --freestanding \
       --mattr=-fp-armv8,-neon -O0 -o kernel-O0.o

# boot.s and vectors.s are assembled with Clang
clang --target=aarch64-unknown-none-elf -c boot.s -o boot.o
clang --target=aarch64-unknown-none-elf -c vectors.s -o vectors.o

# Objects are linked into a bare-metal ELF with lld
ld.lld -T linker.ld -nostdlib -static -o kernel-O0.elf boot.o vectors.o kernel-O0.o
```

Under `--freestanding` the compiler allocates the environment of an escaping closure
(the tasks are closures) with the program's `esk_alloc`, so `alloc.esk` is the kernel's
only heap.
