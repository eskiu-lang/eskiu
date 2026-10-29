# Homebrew formula for Eskiu. This is the source of truth; copy it to the tap repo
# (homebrew-eskiu/Formula/eskiu.rb) and refresh the three sha256 values (macOS arm64,
# Linux x86_64, Linux arm64) from the release's SHA256SUMS on each version bump.
# See packaging/homebrew/README.md.
class Eskiu < Formula
  desc "Self-hosting systems language with a C-style surface and an LLVM backend"
  homepage "https://eskiu-lang.org"
  version "0.14.1"
  license "MIT"

  on_macos do
    on_arm do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.14.1/eskiuc-macos-arm64.tar.gz"
      sha256 "70c3630d8a5ea4cc34c3dfffc3fc93e1b0e780a6a1faea138d832f8c1d894f92"
    end
  end

  on_linux do
    on_intel do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.14.1/eskiuc-linux-x86_64.tar.gz"
      sha256 "b3ccb5fedd92bc6cb0dee1580464d183696aeba97155299ee4ecb9ee920e568d"
    end
    on_arm do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.14.1/eskiuc-linux-arm64.tar.gz"
      sha256 "055e13099ad6797c0a47a5a7a392d80be40fad7781e81879f4c6880dac37d667"
    end
  end

  def install
    # Tarball layout: bin/eskiuc + lib/eskiu/stdlib (+ lib/deps bundled dylibs on macOS).
    # eskiuc finds its stdlib relative to its own path, so keep bin/ and lib/ siblings.
    prefix.install "bin", "lib"
  end

  def caveats
    <<~EOS
      eskiuc compiles programs by shelling out to `clang` to link native output.
      Make sure a C toolchain is installed:
        macOS:  xcode-select --install
        Linux:  brew install llvm   (or your distro's clang / gcc)
    EOS
  end

  test do
    assert_match "Eskiu #{version}", shell_output("#{bin}/eskiuc --version")
  end
end
