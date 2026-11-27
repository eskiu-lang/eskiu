# Homebrew formula for Eskiu. This is the source of truth; copy it to the tap repo
# (homebrew-eskiu/Formula/eskiu.rb) and refresh the sha256 values (macOS arm64 and x86_64, Linux
# x86_64, Linux arm64) from the release's SHA256SUMS on each version bump.
# See packaging/homebrew/README.md.
class Eskiu < Formula
  desc "Self-hosting systems language with a C-style surface and an LLVM backend"
  homepage "https://eskiu-lang.org"
  version "0.17.0"
  license "MIT"

  on_macos do
    on_arm do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.17.0/eskiuc-macos-arm64.tar.gz"
      sha256 "0906636f07b7a89aac64726991125bc149c889c1ff6756ae369ebdd03abb5f2d"
    end
    on_intel do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.17.0/eskiuc-macos-x86_64.tar.gz"
      sha256 "3831be5fc3f9da15694e65466c18612793332e54c7f6fb8750846266113ca4cd"
    end
  end

  on_linux do
    on_intel do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.17.0/eskiuc-linux-x86_64.tar.gz"
      sha256 "14a25b3e9762d0769f305a3f000f45f4c06051ee9a51bfc82bf21ec4d7c33e20"
    end
    on_arm do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.17.0/eskiuc-linux-arm64.tar.gz"
      sha256 "afbb89e05f38c5de843ce5b0e4960deaa87b40a16e5c956bbcf56fcbbad3b0fe"
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
