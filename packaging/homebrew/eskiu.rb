# Homebrew formula for Eskiu. This is the source of truth; copy it to the tap repo
# (homebrew-eskiu/Formula/eskiu.rb) and refresh the sha256 values (macOS arm64 and x86_64, Linux
# x86_64, Linux arm64) from the release's SHA256SUMS on each version bump.
# See packaging/homebrew/README.md.
class Eskiu < Formula
  desc "Self-hosting systems language with a C-style surface and an LLVM backend"
  homepage "https://eskiu-lang.org"
  version "0.16.0"
  license "MIT"

  on_macos do
    on_arm do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.16.0/eskiuc-macos-arm64.tar.gz"
      sha256 "01e9d63d828f28f521d055ea1865db9e529764d24e9c84d743c63229a790252c"
    end
    on_intel do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.16.0/eskiuc-macos-x86_64.tar.gz"
      sha256 "3ac3336e7e187f8eb9efedb15c5d952fb6f5f6567d53f90adce894a1a13f442a"
    end
  end

  on_linux do
    on_intel do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.16.0/eskiuc-linux-x86_64.tar.gz"
      sha256 "29a81da0f2cd7e857edaa52dd792a035d325bdf44067820dffa5c411664c6d9d"
    end
    on_arm do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.16.0/eskiuc-linux-arm64.tar.gz"
      sha256 "dd252d5e91dc751cde5d3ab8d99add5c10a1daa35cfbf469565f1a44133daac2"
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
