# Roadmap

Where Eskiu stands and what comes next. 0.13 is the first public release; per-release detail is in
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

- **Apps.** [ui](https://github.com/eskiu-lang/ui) draws native views on iOS and Android,
  the DOM on the web and a webview window on macOS from one Eskiu codebase. `ui serve`
  shows a change in under a second, `ui open` makes Xcode and Android Studio projects whose
  Run builds the Eskiu code, and `ui release` builds for the App Store and Google Play.
  Packages for location, notifications and the camera build on it.
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
- **EIR.** An IR of its own and native back ends that skip LLVM's code generator
  (`--backend=eir`: ARM64 assembly, C99), passing every run test, with the self-hosted
  compiler building the same EIR byte for byte. What is left:
  [`eir-spec.md` §8](eir-spec.md#8-next-steps).
- **Quality.** Every fix ships with a regression test. CI runs the test suite, the parity
  checks between the two compilers, an `-O0` against `-O2` differential, and fuzzers (a C
  oracle checked against clang, a negative-program corpus, and the stdlib parsers under
  AddressSanitizer).

## Next: 0.15

- **Recoverable exceptions in ui.** 0.14 brought the language's part (`catch (_)` and
  `exception_text()`); next, ui keeps an app running when a block, a handler or an effect
  throws, with `ErrorBoundary` and `ui_on_error`. The plan:
  [`ui-error-recovery.md`](ui-error-recovery.md).

- **Formatter.** Breaking lines that are too long.
- **Build times.** Measure a package with several dependencies, then decide whether
  modules need a cache.
- **Native WebAssembly exceptions.** On emscripten, exceptions run through its
  JavaScript-based runtime, which costs a call into JavaScript at every call that may throw;
  the WebAssembly exception-handling instructions (`-fwasm-exceptions`) would avoid it and
  bring exceptions to WASI. That needs a second lowering of `try` (funclet pads instead of
  `landingpad`) in both compilers.
- **Eskiu in LangArena.** All 50 benchmarks of
  [LangArena](https://github.com/kostya/LangArena) are ported with matching checksums: Eskiu
  runs at C's speed on the 39 algorithmic ones and at about 1.28 times C's time over all 50,
  with byte-identical output from both compilers. Propose it upstream as Goose was added (PR
  #28): a directory with the sources, a Makefile and run scripts, a Docker image from the Linux
  release, and the entry in `benchmarks.rb`.

## Next: 1.0

- **Packages beyond git.** Today the package manager resolves git and path dependencies pinned
  in `eskiu.lock`.
  A registry and version ranges would let packages depend on "1.2 or later" instead of a
  commit.
- **Shipping the self-hosted compiler.** It already matches the C++ one. Making it the
  released binary needs either object emission without clang or accepting clang as a
  runtime requirement; porting the EIR back ends to it is the first
  ([`eir-spec.md` §8](eir-spec.md#8-next-steps)).

## Later, not scheduled

- **Errors that point at the spec.** A rejection whose reason is not obvious from the
  message alone (a closure that escapes, an `escaping` parameter, a `?*T` used without a
  check, a capture written inside its lambda) ends with the section of
  [`spec.md`](../lang/spec.md) that states the rule, such as `(spec §3.2)`. Section numbers
  move when the spec grows, so the messages need anchors that stay put. Both compilers
  print the same text, which `tc_parity.sh` compares byte for byte.

- **Leaner async frames.** Today every local of an async function is stored in its frame;
  storing only the locals that live across an `await` would make frames smaller. This is
  an optimization, not a correctness issue.
- **32-bit Android.** `armeabi-v7a` and `x86` compile, but a C function reached through a
  hand-built fn value (the way `onnx` calls ONNX Runtime's function table) gets
  `size_t` and `c_long` as 64 bits, since the target-width rule applies only to `extern`
  declarations. Bringing fn values under that rule would let packages such as `onnx` build
  for those ABIs. Google Play no longer requires them and few devices still run them.
- **HTTP/3.** By binding an existing QUIC library over `extern` rather than writing QUIC.
  A reverse proxy in front of an Eskiu HTTP/1.1 or HTTP/2 server covers most uses today.
- **ui on the web beyond one page.** Prerendering routes to HTML at build time, then
  server-side rendering with hydration, then server functions the ui tool generates.
- **More ui hosts.** Windows and Linux, and a desktop renderer on `gpu` and `font` without
  a webview.
- **Compile-time `derive`.** Generating code such as serializers from a struct's fields at
  compile time, with no metadata left in the binary. It adds language surface, so it waits
  until after 1.0.

## Non-goals

- Garbage collection. Memory is explicit: `alloc`/`free`, allocators, `defer`.
- Runtime reflection. It would add type metadata to every binary; compile-time generation
  covers the same needs.
