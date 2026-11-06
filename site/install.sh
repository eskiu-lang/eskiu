#!/bin/sh
# Eskiu installer.
#
#   curl -fsSL https://eskiu-lang.org/install.sh | sh
#
# Downloads the prebuilt eskiuc for your platform from GitHub Releases, verifies its
# checksum, and installs it under a prefix. No build toolchain needed to install; eskiuc
# does shell out to clang to link native output, so a C compiler is a runtime requirement.
#
# Environment overrides:
#   ESKIU_VERSION   release tag to install (default: latest), e.g. v0.8.0
#   ESKIU_PREFIX    install prefix (default: /usr/local if writable, else ~/.eskiu)
#
set -eu

REPO="eskiu-lang/eskiu"
VERSION="${ESKIU_VERSION:-latest}"

say()  { printf '%s\n' "$*"; }
info() { printf '\033[1m%s\033[0m\n' "$*" >&2; }
warn() { printf '\033[33mwarning:\033[0m %s\n' "$*" >&2; }
err()  { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }

command -v curl >/dev/null 2>&1 || err "curl is required."
command -v tar  >/dev/null 2>&1 || err "tar is required."

os="$(uname -s)"
arch="$(uname -m)"
case "$os" in
  Darwin)
    case "$arch" in
      arm64|aarch64) asset="eskiuc-macos-arm64.tar.gz" ;;
      x86_64) asset="eskiuc-macos-x86_64.tar.gz" ;;
      *) err "No prebuilt macOS binary for $arch (arm64/x86_64 only). Build from source: https://github.com/$REPO" ;;
    esac ;;
  Linux)
    case "$arch" in
      x86_64|amd64) asset="eskiuc-linux-x86_64.tar.gz" ;;
      arm64|aarch64) asset="eskiuc-linux-arm64.tar.gz" ;;
      *) err "No prebuilt Linux binary for $arch (x86_64/arm64 only). Build from source: https://github.com/$REPO" ;;
    esac ;;
  *)
    err "Unsupported OS: $os. On Windows, use install.ps1 (https://eskiu-lang.org/install.ps1) or the .zip from https://github.com/$REPO/releases." ;;
esac

if [ "$VERSION" = "latest" ]; then
  base="https://github.com/$REPO/releases/latest/download"
else
  base="https://github.com/$REPO/releases/download/$VERSION"
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT INT TERM

info "Downloading $asset ($VERSION) ..."
curl -fSL --proto '=https' --tlsv1.2 -o "$tmp/$asset" "$base/$asset" \
  || err "Download failed: $base/$asset"

# Verify the SHA-256 checksum against the release's SHA256SUMS (best effort: warns and
# continues if the release has no checksum file, e.g. older releases).
if curl -fsSL --proto '=https' -o "$tmp/SHA256SUMS" "$base/SHA256SUMS" 2>/dev/null; then
  line="$(grep " $asset\$" "$tmp/SHA256SUMS" 2>/dev/null || true)"
  if [ -n "$line" ]; then
    info "Verifying checksum ..."
    expected="$(printf '%s' "$line" | awk '{print $1}')"
    if command -v sha256sum >/dev/null 2>&1; then
      actual="$(sha256sum "$tmp/$asset" | awk '{print $1}')"
    else
      actual="$(shasum -a 256 "$tmp/$asset" | awk '{print $1}')"
    fi
    [ "$expected" = "$actual" ] || err "Checksum mismatch for $asset (expected $expected, got $actual)."
  else
    warn "No checksum entry for $asset in SHA256SUMS; skipping verification."
  fi
else
  warn "No SHA256SUMS published for $VERSION; skipping checksum verification."
fi

# Choose an install prefix: /usr/local if we can write it, else a per-user dir.
if [ -n "${ESKIU_PREFIX:-}" ]; then
  prefix="$ESKIU_PREFIX"
elif [ "$(id -u)" = "0" ] || [ -w /usr/local ]; then
  prefix="/usr/local"
else
  prefix="$HOME/.eskiu"
fi

info "Installing to $prefix ..."
mkdir -p "$prefix"
tar -xzf "$tmp/$asset" -C "$prefix"   # the tarball root holds bin/ and lib/

bin="$prefix/bin/eskiuc"
[ -x "$bin" ] || err "Install failed: $bin not found after extraction."

ver="$("$bin" --version 2>/dev/null || true)"
info "Installed ${ver:-eskiuc} to $bin"

# eskiuc links native binaries by shelling out to clang; warn if no C compiler is present.
if ! command -v clang >/dev/null 2>&1 && ! command -v cc >/dev/null 2>&1; then
  warn "eskiuc links native output with clang, which was not found. Install a C toolchain:"
  case "$os" in
    Darwin) warn "  xcode-select --install" ;;
    Linux)  warn "  sudo apt install clang        # or your distro's clang / gcc" ;;
  esac
fi

# PATH guidance (only if the bin dir is not already on PATH).
case ":${PATH}:" in
  *":$prefix/bin:"*) : ;;
  *)
    say ""
    say "Add eskiuc to your PATH (append to your shell profile):"
    say "  export PATH=\"$prefix/bin:\$PATH\""
    ;;
esac

say ""
say "Done. Try:  eskiuc --version"
