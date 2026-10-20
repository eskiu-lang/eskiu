# Homebrew formula for Eskiu. This is the source of truth; copy it to the tap repo
# (homebrew-eskiu/Formula/eskiu.rb) and refresh the three sha256 values (macOS arm64,
# Linux x86_64, Linux arm64) from the release's SHA256SUMS on each version bump.
# See packaging/homebrew/README.md.
class Eskiu < Formula
  desc "Self-hosting systems language with a C-style surface and an LLVM backend"
  homepage "https://eskiu-lang.org"
  version "0.12.0"
  license "MIT"

  on_macos do
    on_arm do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.12.0/eskiuc-macos-arm64.tar.gz"
      sha256 "60203bddb8d7157fde0022d3b8ccb0aee8990889dc1e7ebe1a1e243c8becd01a"
    end
  end

  on_linux do
    on_intel do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.12.0/eskiuc-linux-x86_64.tar.gz"
      sha256 "9b00c935940553281f18cba8b65c2f5701458fb93215e84e3a5e4534392fd4c0"
    end
    on_arm do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.12.0/eskiuc-linux-arm64.tar.gz"
      sha256 "c78d46534a72f737be400fd215dfd6dc00988e500c7d637427a2151ca2bdea54"
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
