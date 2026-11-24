# Homebrew formula for Eskiu. This is the source of truth; copy it to the tap repo
# (homebrew-eskiu/Formula/eskiu.rb) and refresh the sha256 values (macOS arm64 and x86_64, Linux
# x86_64, Linux arm64) from the release's SHA256SUMS on each version bump.
# See packaging/homebrew/README.md.
class Eskiu < Formula
  desc "Self-hosting systems language with a C-style surface and an LLVM backend"
  homepage "https://eskiu-lang.org"
  version "0.16.3"
  license "MIT"

  on_macos do
    on_arm do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.16.3/eskiuc-macos-arm64.tar.gz"
      sha256 "d237d26e914beb18bfbb0ecfe6631da27154ebd286a2064a24c724affd2da281"
    end
    on_intel do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.16.3/eskiuc-macos-x86_64.tar.gz"
      sha256 "8098cfad78f082aed374e755304656b30765099043da541343194a59042344be"
    end
  end

  on_linux do
    on_intel do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.16.3/eskiuc-linux-x86_64.tar.gz"
      sha256 "545c61df119f58678756c912e04f3c30a050e730b8093227836cca45ac1b3e62"
    end
    on_arm do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.16.3/eskiuc-linux-arm64.tar.gz"
      sha256 "d68dc6b5c92d955bf8543779bcf1fb9a28f1e31f7abd3ffe7ffbc1bbd0b938b9"
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
