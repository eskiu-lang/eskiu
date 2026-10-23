# Homebrew formula for Eskiu. This is the source of truth; copy it to the tap repo
# (homebrew-eskiu/Formula/eskiu.rb) and refresh the three sha256 values (macOS arm64,
# Linux x86_64, Linux arm64) from the release's SHA256SUMS on each version bump.
# See packaging/homebrew/README.md.
class Eskiu < Formula
  desc "Self-hosting systems language with a C-style surface and an LLVM backend"
  homepage "https://eskiu-lang.org"
  version "0.14.0"
  license "MIT"

  on_macos do
    on_arm do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.14.0/eskiuc-macos-arm64.tar.gz"
      sha256 "ced8ed271e0db5b8ea09c4ce6487049ffa1d7eb102d3758624c2c16c642f0bfd"
    end
  end

  on_linux do
    on_intel do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.14.0/eskiuc-linux-x86_64.tar.gz"
      sha256 "4d6bb09c9995991e32c178fd5d4bcdd85e4652c750ce4f578c386fd5e8f2eb50"
    end
    on_arm do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.14.0/eskiuc-linux-arm64.tar.gz"
      sha256 "0824bbd90913bed7bc85254fc2bf4c0ee1e76c095dcd6aa7380097f7e3a6eb19"
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
