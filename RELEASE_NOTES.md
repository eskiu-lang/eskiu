# Eskiu 0.14.0

Eskiu 0.14 catches any exception and fetches native libraries for phones and cross builds.

## What is new

- **`catch (_)`** catches any exception without binding it, as the last catch of a `try`.
  Inside a catch, **`exception_text()`** describes what was caught (`int 7`,
  `string "boom"`), also across an `await`:

  ```eskiu
  try {
      load_config(path);
  } catch (string e) {
      printf("bad config: %s\n", e);
  } catch (_) {
      printf("unexpected: %s\n", exception_text());
  }
  ```

- **`[native.NAME]` for iOS, Android and `--target` builds.** A package's prebuilt
  libraries now have keys for `ios-arm64`, `ios-arm64-simulator`, `ios-x86_64-simulator`,
  `android-arm64` and `android-x86_64`, and a cross build fetches and links the archive of
  its target, as a host build does. The `gpu` package uses it for WebGPU on phones.
- **Fixed:** `--target x86_64-w64-mingw32` passed structs to C functions with the wrong
  convention, so a call into a DLL could crash; every target triple is now normalized.

Code that compiles with 0.13 compiles with 0.14. The full list is in the
[changelog](https://eskiu-lang.org/changelog.html).

## Install

**macOS (Apple Silicon) and Linux (x86-64, arm64)**

```bash
curl -fsSL https://eskiu-lang.org/install.sh | sh
eskiuc --version
```

Or download a tarball below and unpack it into a prefix (`tar -xzf eskiuc-<platform>.tar.gz -C /usr/local`).

**Windows (x86-64)**

```powershell
Expand-Archive eskiuc-windows-x86_64.zip -DestinationPath C:\eskiu   # then add C:\eskiu\bin to PATH
eskiuc --version
```

Or build from source (LLVM 21 or newer, LLVM 22 recommended, and CMake 3.20+):

```bash
git clone https://github.com/eskiu-lang/eskiu
cd eskiu && cmake -S . -B build && cmake --build build
```

## Known issues

- `eskiuc repl` is not available on Windows in this release. `eskiuc run` works there.

The full summary is in the [changelog](https://eskiu-lang.org/changelog.html).
