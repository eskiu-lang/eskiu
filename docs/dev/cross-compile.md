# Cross-Compiling

`eskiuc` emits objects for a target other than the build host. Set the triple with
`--target`; three companion flags tune the machine and ABI:

| Flag        | Purpose                                                                          |
| ----------- | -------------------------------------------------------------------------------- |
| `--target`  | Target triple (e.g. `x86_64-linux-gnu`, `armv6k-none-eabihf`). Empty = host.     |
| `--mcpu`    | Target CPU passed to LLVM (e.g. `mpcore`). Empty = the target baseline, native builds included (as in clang): `apple-m1` on arm64 Apple targets, `generic` elsewhere. |
| `--mattr`   | LLVM feature string, `-mattr` syntax (e.g. `+vfp2`).                              |
| `--reloc`   | Relocation model: `pic` (default), `static`, or `dynamic-no-pic`.                |

The registered backends are AArch64, x86 (x86-64 and 32-bit i686), 32-bit ARM and
WebAssembly (wasm32). For a cross target, emit an object file (`-c`, or an output ending in
`.o`) and link it with a cross toolchain for the same platform. An executable output makes
`eskiuc` link through `$CC`. When `$CC` is not set, a WebAssembly, Android or iOS triple
picks that platform's driver (see the sections below); any other target uses `cc`, `clang`
or `gcc`, which only works for another target when one of them is a C driver for it.

The output's extension decides what is built: an executable, an object (`.o`, `.obj`), a
static library (`.a`, `.lib`: the object in an archive, written by `eskiuc` itself) or a
shared library (`.so`, `.dylib`, `.dll`: linked with `-shared`; objects are
position-independent by default). A library is how an Eskiu module goes into an app that
another build system links, such as an Xcode project, a Gradle module or an emscripten
build.

## Platform macros

The preprocessor predefines a platform macro so `import`ed stdlib code can branch per OS:

- An explicit `--target` containing `linux` defines `__linux__`; one containing `apple`,
  `darwin`, or `macos` defines `__APPLE__`; one containing `windows`, `win32`, or `mingw`
  defines `_WIN32` (plus `_WIN64` for a 64-bit arch).
- A **non-hosted** triple (OS `none`, such as `armv6k-none-eabihf`) defines neither. Bare
  metal has no host OS, so portable code guards that path explicitly.
- With no `--target`, the macro follows the build host.

An architecture macro follows the same rule (the `--target` triple, else the build host):
`__aarch64__` for AArch64, `__x86_64__` for x86-64, `__arm__` for 32-bit ARM. At most one
is defined.

Three more families get the macros clang predefines for them: a `wasm32` triple defines
`__wasm__` and `__wasm32__` (plus `__EMSCRIPTEN__` for emscripten and `__wasi__` for WASI),
an Android triple defines `__ANDROID__` next to `__linux__`, and an iOS triple defines
`TARGET_OS_IPHONE` and `TARGET_OS_IOS` next to `__APPLE__` (plus `TARGET_IPHONE_SIMULATOR`
for a simulator).

`--freestanding` additionally predefines `__ESKIU_FREESTANDING__` and routes `alloc`/`free`
to caller-supplied `esk_alloc`/`esk_free` instead of libc.

## Hard-float ARM

A triple ending in `hf` (e.g. `armv6k-none-eabihf`) selects the hard-float ABI. LLVM then
stamps the `Tag_ABI_VFP_args` build attribute on the object, which a hard-float linker
requires to mix the object with hard-float libraries. Float arguments pass in VFP
registers (`s0`, `s1`, ...) rather than core registers.

Verify it on an emitted object:

```bash
llvm-readobj -A out.o | grep -A1 'VFP_args'
#   TagName: ABI_VFP_args
#   Description: AAPCS VFP
```

## Example: Nintendo 3DS

The 3DS is an ARM11 MPCore (`armv6k`) with VFPv2, hard-float, and a homebrew loader that
applies static relocations. Compile an Eskiu source to a 3DS object:

```bash
eskiuc lib.esk -o lib.o \
    --target armv6k-none-eabihf --mcpu mpcore --mattr +vfp2 \
    --reloc static --freestanding
```

Then link it with the [devkitARM](https://devkitpro.org/) toolchain, alongside a C entry
point and libctru, and package the result:

```bash
# C side (entry point + libctru calls), same arch flags:
arm-none-eabi-gcc -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft \
    -I$DEVKITPRO/libctru/include -c main.c -o main.o

# Link the C object + the Eskiu object into a 3DS ELF, then wrap it as .3dsx:
arm-none-eabi-gcc -specs=3dsx.specs -march=armv6k -mtune=mpcore -mfloat-abi=hard \
    main.o lib.o -L$DEVKITPRO/libctru/lib -lctru -lm -o app.elf
3dsxtool app.elf app.3dsx
```

The link succeeds only when both objects agree on the float ABI, which is why the `hf`
triple (and the resulting `Tag_ABI_VFP_args`) matters: without it the linker rejects the
Eskiu object against hard-float libctru.

Eskiu declarations compile to C-ABI symbols, so a `main.c` can call an Eskiu function by
declaring it `extern`. In the other direction, a struct passed to or returned from an
`extern` C function by value follows the target's C calling convention (AArch64, x86-64
System V, Windows x64, 32-bit ARM including the hard-float HFA rules, and 32-bit x86); see
[`abi.md`](abi.md). The self-hosted compiler passes `--mcpu`, `--mattr` and `--reloc` on to
clang, which assembles its IR, so it builds for these targets too.

## Example: Windows (x86-64)

The x86 backend emits COFF objects for a Windows triple with no extra flags; LLVM lowers
the Microsoft x64 calling convention (first integer arguments in `rcx`/`rdx`/`r8`/`r9`)
from the triple:

```bash
eskiuc app.esk -o app.obj --target x86_64-pc-windows-gnu --freestanding
file app.obj    # Intel amd64 COFF object file
```

Link with a Windows linker. LLVM's `lld-link` needs an import library for any DLL the
program calls; generate one from a `.def` with `llvm-dlltool` (both ship with LLVM), so no
Windows SDK is required to link:

```bash
printf 'LIBRARY kernel32.dll\nEXPORTS\nExitProcess\nGetStdHandle\nWriteFile\n' > kernel32.def
llvm-dlltool -m i386:x86-64 -d kernel32.def -l kernel32.lib
lld-link /entry:mainCRTStartup /subsystem:console app.obj kernel32.lib /out:app.exe
```

A freestanding program provides its own entry point (`/entry:`) and calls Win32 directly.
A hosted program instead links the MinGW or MSVC C runtime and uses the ordinary `main`.
The resulting `.exe` runs on Windows, or under `wine` for local testing.

Exceptions (`try`/`throw`/`catch`) work on a mingw target: the compiler emits the SEH EH
personality (`__gxx_personality_seh0`) there instead of the Itanium `__gxx_personality_v0`,
so linking with a mingw `g++` (which supplies the `__cxa_*` runtime and the SEH unwinder)
produces a working `.exe`. This is exercised end to end on a native Windows runner in
`.github/workflows/windows.yml`.

The OS-level stdlib modules carry Windows backends: `<sysheap>` maps pages with
`VirtualAlloc`/`VirtualFree` (no `mmap`), `<time>` uses the Win32 clocks (`GetTickCount64`,
`GetSystemTimeAsFileTime`, `Sleep`), `<threading>` links against winpthreads, and `<net>`
(blocking TCP: listen/accept/connect/send/recv) uses the Winsock backend (`WSAStartup` +
`closesocket`; `ws2_32` is linked automatically through the module's `#pragma link`). A blocking, thread-per-connection server works on Windows
with these. Each has an end-to-end check on a native Windows runner in
`.github/workflows/windows.yml`.

The **async** stack runs on Windows too. `<eventloop>` swaps its epoll/kqueue reactor for
`WSAPoll` over a user-space pollfd set (rebuilt each wait, since `WSAPoll` is stateless), and
`<net_async>` uses `ioctlsocket(FIONBIO)` + `recv`/`send` + `WSAGetLastError` in place of
`fcntl(O_NONBLOCK)` + `read`/`write` + errno. One Windows subtlety: a `SOCKET` is not a small
sequential fd (handles like 236 are normal), so the loop's fd-indexed slot table grows to fit
the largest handle rather than assuming a dense `[0, max_fds)` range. A non-blocking socket
round-trip over the reactor is checked end to end on the native Windows runner.

## C types whose width depends on the target

C's `size_t`, `ssize_t` and `long` change width between targets: 32 bits on wasm32,
32-bit ARM and i686, and `long` also on Windows. Eskiu sees each of them as an `int64`;
spelled `size_t`, `ssize_t` or `c_long` (from `import <ctypes>;`) in an `extern`'s
parameters or result, the declaration uses the target's C type, an argument is converted to
it and the result widened back. The standard library declares the C functions it uses this
way, so `strlen`, `memcpy`, `malloc`, `read` or `snprintf` are right on every target. A
program that declares such a function itself should do the same; an `int64` there is wrong
on a 32-bit target (on WebAssembly the call traps on the signature mismatch).

## WebAssembly

`--target wasm32-unknown-emscripten` builds for [emscripten](https://emscripten.org), the
toolchain most web projects use. With no `$CC`, `eskiuc` links through `emcc`, so the
output's extension chooses what emcc writes (`.js` with its `.wasm`, `.html`, or `.wasm`),
and `--link-arg` passes emcc's settings:

```bash
eskiuc app.esk --target wasm32-unknown-emscripten -o app.js --link-arg=-sEXIT_RUNTIME=1
node app.js
```

To use Eskiu inside an existing emscripten project, build a static library and link it like
any other: `eskiuc tracker.esk --target wasm32-unknown-emscripten -o libtracker.a`, then
`emcc main.cpp libtracker.a ...`. Eskiu functions are C symbols, so C++ (an embind
binding, for instance) declares them `extern "C"` and calls them; JavaScript reaches one
exported with `-sEXPORTED_FUNCTIONS=_name`.

Other wasm32 triples (`wasm32-wasip1`, `wasm32-unknown-unknown`) link with the LLVM
toolchain's own `clang --target=...`; WASI needs a sysroot (`--link-arg=--sysroot=...`),
and `wasm32-unknown-unknown` is normally built `--freestanding`.

The standard library works there as on other POSIX systems, with an event loop over
`poll()`, so futures, timers and channels run under node. Exceptions work on emscripten
through its JavaScript-based runtime: the backend lowers each `invoke` and `landingpad` to
calls into it (clang's `-fexceptions` under emcc), and a program that throws or catches is
linked with `-sDISABLE_EXCEPTION_CATCHING=0`. On the other wasm32 triples `try` and `throw`
are a located error. Threads need emscripten's `-pthread` build. `tests/wasm/check.sh` runs the test corpus this way (emcc and node on
`PATH`), skipping the tests listed in `tests/wasm/skip.txt` with the reason for each.

## Android

An Android triple carries the API level: `aarch64-linux-android24`,
`armv7a-linux-androideabi24`, `x86_64-linux-android24`. With no `$CC`, `eskiuc` links with
the NDK's clang, from `$ANDROID_NDK_HOME`, `$ANDROID_NDK_ROOT`, or the newest NDK under
the SDK (`$ANDROID_HOME/ndk`, or the SDK manager's default directory). A shared library is
what an app loads (`System.loadLibrary`, or a React Native module's native part):

```bash
eskiuc tracker.esk --target aarch64-linux-android24 -o libtracker.so
```

pthread is part of Android's libc, so a program with threads links no `-lpthread`, and one
that throws links the NDK's `libc++_static` and `libc++abi`. `PLATFORM=android
tests/mobile/check.sh` runs the test corpus on an emulator or device through `adb`.

## iOS

`arm64-apple-ios15.0` builds for devices and `arm64-apple-ios15.0-simulator` (or
`x86_64-apple-ios15.0-simulator`) for the simulator. With no `$CC`, `eskiuc` links through
`xcrun -sdk iphoneos clang` (`iphonesimulator` for a simulator triple). An app embeds a
static library, one per slice, which `xcodebuild -create-xcframework` can combine:

```bash
eskiuc tracker.esk --target arm64-apple-ios15.0 -o ios/libtracker.a
eskiuc tracker.esk --target arm64-apple-ios15.0-simulator -o sim/libtracker.a
xcodebuild -create-xcframework -library ios/libtracker.a -library sim/libtracker.a \
    -output Tracker.xcframework
```

The C ABI is the Darwin AArch64 one, as on macOS. `tests/mobile/check.sh` runs the test
corpus on a booted simulator (`xcrun simctl spawn`).

## Windows GUI programs

A program that opens its own window, such as one using the `gpu` package, links for the GUI
subsystem with `--subsystem=windows`, so Windows does not open a console for it. The entry
point stays `main`: the driver passes `-mwindows` to a mingw link, and `/subsystem:windows`
with `/entry:mainCRTStartup` to an MSVC one. On other targets the option has no effect.
