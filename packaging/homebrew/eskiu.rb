# Homebrew formula for Eskiu. This is the source of truth; copy it to the tap repo
# (homebrew-eskiu/Formula/eskiu.rb) and refresh the sha256 values (macOS arm64 and x86_64, Linux
# x86_64, Linux arm64) from the release's SHA256SUMS on each version bump.
# See packaging/homebrew/README.md.
class Eskiu < Formula
  desc "Self-hosting systems language with a C-style surface and an LLVM backend"
  homepage "https://eskiu-lang.org"
  version "0.16.1"
  license "MIT"

  on_macos do
    on_arm do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.16.1/eskiuc-macos-arm64.tar.gz"
      sha256 "edef49acf4d3e488ed6dcf9934be3cc44907c078f77862080f085afc3dc58914"
    end
    on_intel do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.16.1/eskiuc-macos-x86_64.tar.gz"
      sha256 "b3c6c1a00ca799e5c016f161309f2291004447d5ad21ff56ef08d3046f7abccb"
    end
  end

  on_linux do
    on_intel do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.16.1/eskiuc-linux-x86_64.tar.gz"
      sha256 "6364605821bddb1800bbda8dca9601da84ad60309f65a635253dc599ec25d717"
    end
    on_arm do
      url "https://github.com/eskiu-lang/eskiu/releases/download/v0.16.1/eskiuc-linux-arm64.tar.gz"
      sha256 "ccfa01a8f1063388004b8144dd8f5871d5536b9eaeff51d325f6c8fee1204077"
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
