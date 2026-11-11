# Homebrew formula for Eskiu. This is the source of truth; copy it to the tap repo
# (homebrew-eskiu/Formula/eskiu.rb) and refresh the sha256 values (macOS arm64 and x86_64, Linux
# x86_64, Linux arm64) from the release's SHA256SUMS on each version bump.
# See packaging/homebrew/README.md.
class Eskiu < Formula
  desc "Self-hosting systems language with a C-style surface and an LLVM backend"
  homepage "https://eskiu-lang.org"
  version "0.15.1"
  license "MIT"

  on_macos do
    on_arm do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.15.1/eskiuc-macos-arm64.tar.gz"
      sha256 "984a275e38b9effa60aac7cb9719bfa777d7999047db3079694963c6664f6bd7"
    end
    on_intel do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.15.1/eskiuc-macos-x86_64.tar.gz"
      sha256 "9710b0208f0133cf37a6cfcfc4620ccbdafa9624514bf26c569883ef18dec7a7"
    end
  end

  on_linux do
    on_intel do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.15.1/eskiuc-linux-x86_64.tar.gz"
      sha256 "8f90f5ab02f491a22e07f0b40f55b26389f6cb55408bf20bfa3dcc7bf4c7122f"
    end
    on_arm do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.15.1/eskiuc-linux-arm64.tar.gz"
      sha256 "d8183943924804ecfb7148e12d25bcecb944737f19688f2f25d5f9ff126f039d"
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
