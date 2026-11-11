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
      sha256 "b806b1a8ce74047d0cfa6a58c0833e11fe8ec174dc9be03ebef8c22e0491a991"
    end
    on_intel do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.15.1/eskiuc-macos-x86_64.tar.gz"
      sha256 "10f5068e10fb32dae6024bde7d1fd44d3b3efe854fff2706686674daee4e8abb"
    end
  end

  on_linux do
    on_intel do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.15.1/eskiuc-linux-x86_64.tar.gz"
      sha256 "f689c141d314978f4993bdcc93ba16528bb55d3c3548cf7af7d4dffd3f8f2bc0"
    end
    on_arm do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.15.1/eskiuc-linux-arm64.tar.gz"
      sha256 "ac65d134f7e6286a1ff323c35e7351e07fd9deafd5fb691b7bb2dd20d6e8b657"
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
