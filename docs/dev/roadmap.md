# Roadmap

Where Eskiu stands and what comes next. 0.12 is the first public release; per-release detail is in
[`CHANGELOG.md`](../../CHANGELOG.md), and what can change between releases is in
[stability.md](../lang/stability.md).

## Goal

Compute-heavy services are often split across several languages: C for the hot paths, Go
for concurrency, C++ for libraries, Python for glue. Eskiu aims to cover that ground with one
language that compiles to native code through LLVM, calls any C library directly, manages
memory explicitly, and still runs a file directly with `eskiuc run`.

The work has two stages. The first, a systems foundation that can do what C does, is
complete. The second is to make the types that high-throughput services work with
first-class in the language without giving up general systems use.

## Where it is today

- **Tooling.** `eskiuc run` for scripts, `eskiuc repl` for trying code, `eskiuc new` /
  `add` / `build` / `test` for packages, `eskiuc fmt`, and editor support in VS Code-based
  editors and JetBrains IDEs through the same compiler modes.

- **Language.** C-style syntax and C integer semantics, structs with methods, structural
  interfaces, monomorphized generics with constraints, sum types with exhaustive `match`,
  closures with escape analysis, exceptions, `defer`/`errdefer`, slices, checked nullable
  pointers, operator overloading, inline assembly, async/await.
- **Standard library.** Collections, strings, JSON, regex, time, random, allocators,
  threads and atomics, an async runtime, and HTTP/1.1, HTTP/2 and TLS servers.
- **Targets.** Native on macOS arm64, Linux x86-64 and arm64, and Windows x86-64, with
  prebuilt binaries for all four. Cross-compilation to WebAssembly (emscripten and WASI),
  Android and iOS, with static and shared library outputs for app builds, to 32-bit ARM
  (verified on a Nintendo 3DS) and 32-bit x86, and freestanding builds for bare metal (an
  ARM64 kernel boots in QEMU). The C ABI is followed on every target. See
  [`cross-compile.md`](cross-compile.md).
- **Self-hosting.** The compiler is also written in Eskiu (`selfhost/`). It reaches a
  three-stage bootstrap fixpoint, behaves like the C++ compiler over the whole test corpus,
  and is built and checked by CI as `eskiuc-esk`. The C++ `eskiuc` remains the released
  binary because it bundles LLVM. See [`self-hosting.md`](self-hosting.md).
- **Quality.** Every fix ships with a regression test. CI runs the test suite, the parity
  checks between the two compilers, an `-O0` against `-O2` differential, and fuzzers (a C
  oracle checked against clang, a negative-program corpus, and the stdlib parsers under
  AddressSanitizer).

## Next: 0.13

- **Formatter.** Breaking lines that are too long.
- **Build times.** Measure a package with several dependencies, then decide whether
  modules need a cache.
- **Native WebAssembly exceptions.** On emscripten, exceptions run through its
  JavaScript-based runtime, which costs a call into JavaScript at every call that may throw;
  the WebAssembly exception-handling instructions (`-fwasm-exceptions`) would avoid it and
  bring exceptions to WASI. That needs a second lowering of `try` (funclet pads instead of
  `landingpad`) in both compilers.
- **Native libraries in packages.** A package that links a native library (such as `gpu`
  with wgpu-native and SDL3, or an ONNX Runtime binding) needs it installed by hand today
  (`brew install`, a release archive) and its directory given as `eskiuc build -L...`;
  `eskiuc run` inside a package passes its arguments to the program, so it cannot take one.
  The manifest should declare the prebuilt library per platform (URL and sha256), so that
  `eskiuc fetch` downloads and verifies it into the package cache, and the link directory
  and run-time path are set by `eskiuc build` and `eskiuc run` with no flags.

## Next: 1.0

- **Packages beyond git.** Today the package manager resolves git and path dependencies pinned
  in `eskiu.lock`.
  A registry and version ranges would let packages depend on "1.2 or later" instead of a
  commit.
- **Shipping the self-hosted compiler.** It already matches the C++ one. Making it the
  released binary needs either object emission without clang or accepting clang as a
  runtime requirement.

## Later, not scheduled

- **Leaner async frames.** Today every local of an async function is stored in its frame;
  storing only the locals that live across an `await` would make frames smaller. This is
  an optimization, not a correctness issue.
- **Mobile.** Android (`.so` over JNI, built against the NDK) and iOS (a static library
  called from Swift through the C ABI). The LLVM backends already cover both; the work is
  linking, stdlib branches and packaging.
- **HTTP/3.** By binding an existing QUIC library over `extern` rather than writing QUIC.
  A reverse proxy in front of an Eskiu HTTP/1.1 or HTTP/2 server covers most uses today.
- **Compile-time `derive`.** Generating code such as serializers from a struct's fields at
  compile time, with no metadata left in the binary. It adds language surface, so it waits
  until after 1.0.

## Non-goals

- Garbage collection. Memory is explicit: `alloc`/`free`, allocators, `defer`.
- Runtime reflection. It would add type metadata to every binary; compile-time generation
  covers the same needs.
