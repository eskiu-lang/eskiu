# Homebrew formula for Eskiu. This is the source of truth; copy it to the tap repo
# (homebrew-eskiu/Formula/eskiu.rb) and refresh the sha256 values (macOS arm64 and x86_64, Linux
# x86_64, Linux arm64) from the release's SHA256SUMS on each version bump.
# See packaging/homebrew/README.md.
class Eskiu < Formula
  desc "Self-hosting systems language with a C-style surface and an LLVM backend"
  homepage "https://eskiu-lang.org"
  version "0.16.2"
  license "MIT"

  on_macos do
    on_arm do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.16.2/eskiuc-macos-arm64.tar.gz"
      sha256 "786ede0634872c73fd851d4860e2fc017113ba4d4b6b1b3f51bcabf43ee3caff"
    end
    on_intel do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.16.2/eskiuc-macos-x86_64.tar.gz"
      sha256 "2bf3f2b847d0c437f89654a5cd588a9d3a474db11d20cf2dfec5f1ff75fa6af3"
    end
  end

  on_linux do
    on_intel do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.16.2/eskiuc-linux-x86_64.tar.gz"
      sha256 "0b87682f1030fcf6b1c47e6305f643d9d3cfa77be944e8a9913029136171de7a"
    end
    on_arm do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.16.2/eskiuc-linux-arm64.tar.gz"
      sha256 "8510517235092820dbab6f6a8d384499e1168c8096326f42c212a7f04d986a8a"
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
