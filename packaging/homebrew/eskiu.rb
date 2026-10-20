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
      sha256 "4ada24c3d14cc68f37ec6307cdf57bd825f7772342fa9d497049511f1ccf8b6f"
    end
  end

  on_linux do
    on_intel do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.12.0/eskiuc-linux-x86_64.tar.gz"
      sha256 "a145c8b41689d187aa595bad382e23bf0f441d9cbae1eba0033786e316b8cb6b"
    end
    on_arm do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.12.0/eskiuc-linux-arm64.tar.gz"
      sha256 "c576451b2d4199255ca259a35e19632381c295edfead3ae903ed781292faf03d"
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
