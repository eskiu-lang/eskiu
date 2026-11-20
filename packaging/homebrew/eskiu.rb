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
      sha256 "378703f1e6c1e08242c69ed6e6424e16aab14c5f300ddcaa1356a3e4c6640f44"
    end
    on_intel do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.16.2/eskiuc-macos-x86_64.tar.gz"
      sha256 "6a0bda97defe8ca33c618351908e091c8dd4852dbf1821b22fd9deade3ed4f09"
    end
  end

  on_linux do
    on_intel do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.16.2/eskiuc-linux-x86_64.tar.gz"
      sha256 "366a2a568d4c1e5b9cf9fa7dbdec9b7b6a27639dcde4da72f79604dd0989927b"
    end
    on_arm do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.16.2/eskiuc-linux-arm64.tar.gz"
      sha256 "08226891cb6c90d2c4efaa49a1d136dbe481bfb2b93f3993d6ccb5c109889a3e"
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
