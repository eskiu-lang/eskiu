# Homebrew formula for Eskiu. This is the source of truth; copy it to the tap repo
# (homebrew-eskiu/Formula/eskiu.rb) and refresh the sha256 values (macOS arm64 and x86_64, Linux
# x86_64, Linux arm64) from the release's SHA256SUMS on each version bump.
# See packaging/homebrew/README.md.
class Eskiu < Formula
  desc "Self-hosting systems language with a C-style surface and an LLVM backend"
  homepage "https://eskiu-lang.org"
  version "0.15.0"
  license "MIT"

  on_macos do
    on_arm do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.15.0/eskiuc-macos-arm64.tar.gz"
      sha256 "e7c49b920221d4ccf1a3763245447c9f7de2fbd37bcd0aad654fb6635fb13b84"
    end
    on_intel do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.15.0/eskiuc-macos-x86_64.tar.gz"
      sha256 "ecbf88f11fb059c697be72ffb6be42e43336aee80bdf422a9f132d43cad2a56e"
    end
  end

  on_linux do
    on_intel do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.15.0/eskiuc-linux-x86_64.tar.gz"
      sha256 "2a0cd660dbca783d8ea6848a705256682248f2395cf26a1333ec8a1885f8ad1f"
    end
    on_arm do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.15.0/eskiuc-linux-arm64.tar.gz"
      sha256 "6ba72ee21dc223dbb2d61ec39efde4ce089548400b1547360261555a31d5e034"
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
