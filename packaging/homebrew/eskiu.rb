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
      sha256 "7f5916930d1b1d8d5479c5fd2eeeeaa800255d2685728ec4fecc78b65893a655"
    end
  end

  on_linux do
    on_intel do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.12.0/eskiuc-linux-x86_64.tar.gz"
      sha256 "ddf28e30877f88ec27f6ef06b8ccf5e3fdd329668e50d3bff0b74044f9cdb9e8"
    end
    on_arm do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.12.0/eskiuc-linux-arm64.tar.gz"
      sha256 "85218d9f93387c9dc3c0c426fdfc3dcf839d69bfa572bd3a4e8321958cdeeb7a"
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
