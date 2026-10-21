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
      sha256 "6d969053946d7bdcd1abd0c936ad0ed2a5cd0154cbbeefc3ebc60237f114e38a"
    end
  end

  on_linux do
    on_intel do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.12.0/eskiuc-linux-x86_64.tar.gz"
      sha256 "8fbe8af9cc700aa891887dca7de5d9900022158db8fbe2559d2f5771d4836cc1"
    end
    on_arm do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.12.0/eskiuc-linux-arm64.tar.gz"
      sha256 "5b16b994fac17b48cdd883f6b105168e46f9d20cbc41207e86181cebdd9c836b"
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
