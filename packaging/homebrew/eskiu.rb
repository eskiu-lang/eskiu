# Homebrew formula for Eskiu. This is the source of truth; copy it to the tap repo
# (homebrew-eskiu/Formula/eskiu.rb) and refresh the three sha256 values (macOS arm64,
# Linux x86_64, Linux arm64) from the release's SHA256SUMS on each version bump.
# See packaging/homebrew/README.md.
class Eskiu < Formula
  desc "Self-hosting systems language with a C-style surface and an LLVM backend"
  homepage "https://eskiu-lang.org"
  version "0.13.0"
  license "MIT"

  on_macos do
    on_arm do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.13.0/eskiuc-macos-arm64.tar.gz"
      sha256 "2c96ecce12bf4936732a175c6802c2c9b91fed72104879f691b4c22bfdf4393a"
    end
  end

  on_linux do
    on_intel do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.13.0/eskiuc-linux-x86_64.tar.gz"
      sha256 "33ae65ef2716de173abfa713783d8b56d9d635bdde7bfef181ac12063b62574f"
    end
    on_arm do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.13.0/eskiuc-linux-arm64.tar.gz"
      sha256 "b74c79fb1e7908b502b69203b9287fa6c0b2bcb6aa80114f635057a5c00e8e66"
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
