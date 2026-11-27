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
      sha256 "ae33105f82dbff788ef48d8f9968eaf2647ebf747b013384ab5f760f51ce92ae"
    end
    on_intel do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.17.0/eskiuc-macos-x86_64.tar.gz"
      sha256 "0cf44d762a123ab86f4f3c5eeb4331c9efd08baff195f72f31957a27cd9730c3"
    end
  end

  on_linux do
    on_intel do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.17.0/eskiuc-linux-x86_64.tar.gz"
      sha256 "4f690b639149f793b036b9c2792fcb8f6c822870e12372c412e2ff65ce9e6d7e"
    end
    on_arm do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.17.0/eskiuc-linux-arm64.tar.gz"
      sha256 "17eddc686489933866f54abe1a5b76e14843f40918fb45733bb6c62a0a7e14fa"
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
