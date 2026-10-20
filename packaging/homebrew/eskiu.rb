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
      sha256 "1f358096375030d40a56155f4726338e622e9e4ed7725fead6500c29bf1439df"
    end
  end

  on_linux do
    on_intel do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.12.0/eskiuc-linux-x86_64.tar.gz"
      sha256 "7bbc60590b3485946fe0ac06e7b3eca28c7d9daa3ef464a5e9d2b8b61073b2c2"
    end
    on_arm do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.12.0/eskiuc-linux-arm64.tar.gz"
      sha256 "99fd1a451753247ee6929bd0135ac9cb3e2aaa56466e072a050bd0de81831d1f"
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
