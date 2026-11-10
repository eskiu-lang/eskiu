#!/bin/sh
# Eskiu installer.
#
#   curl -fsSL https://eskiu-lang.org/install.sh | sh
#   curl -fsSL https://eskiu-lang.org/install.sh | sh -s -- --with-ui   # also the ui tool
#
# Downloads the prebuilt eskiuc for your platform from GitHub Releases, verifies its
# checksum, and installs it under a prefix. No build toolchain needed to install; eskiuc
# does shell out to clang to link native output, so a C compiler is a runtime requirement.
#
# Environment overrides:
#   ESKIU_VERSION   release tag to install (default: latest), e.g. v0.8.0
#   ESKIU_PREFIX    install prefix (default: /usr/local if writable, else ~/.eskiu)
#   ESKIU_WITH_UI   1 also installs the ui tool (eskiu-lang/ui), as --with-ui does
#   UI_VERSION      ui release tag to install (default: latest)
#
set -eu

REPO="eskiu-lang/eskiu"
VERSION="${ESKIU_VERSION:-latest}"
UI_REPO="eskiu-lang/ui"
UI_VERSION="${UI_VERSION:-latest}"
WITH_UI="${ESKIU_WITH_UI:-0}"
for a in "$@"; do
  case "$a" in
    --with-ui) WITH_UI=1 ;;
    *) printf 'unknown option: %s\n' "$a" >&2; exit 2 ;;
  esac
done

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
      arm64|aarch64) plat="macos-arm64" ;;
      x86_64) plat="macos-x86_64" ;;
      *) err "No prebuilt macOS binary for $arch (arm64/x86_64 only). Build from source: https://github.com/$REPO" ;;
    esac ;;
  Linux)
    case "$arch" in
      x86_64|amd64) plat="linux-x86_64" ;;
      arm64|aarch64) plat="linux-arm64" ;;
      *) err "No prebuilt Linux binary for $arch (x86_64/arm64 only). Build from source: https://github.com/$REPO" ;;
    esac ;;
  *)
    err "Unsupported OS: $os. On Windows, use install.ps1 (https://eskiu-lang.org/install.ps1) or the .zip from https://github.com/$REPO/releases." ;;
esac

asset="eskiuc-$plat.tar.gz"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT INT TERM

# fetch REPO VERSION ASSET: downloads a release asset into $tmp and checks it against the
# release's SHA256SUMS (best effort: warns and goes on when the release has none).
fetch() {
  if [ "$2" = "latest" ]; then
    url="https://github.com/$1/releases/latest/download"
  else
    url="https://github.com/$1/releases/download/$2"
  fi
  info "Downloading $3 ($2) ..."
  curl -fSL --proto '=https' --tlsv1.2 -o "$tmp/$3" "$url/$3" || err "Download failed: $url/$3"
  rm -f "$tmp/SHA256SUMS"
  if curl -fsSL --proto '=https' -o "$tmp/SHA256SUMS" "$url/SHA256SUMS" 2>/dev/null; then
    line="$(grep " $3\$" "$tmp/SHA256SUMS" 2>/dev/null || true)"
    if [ -n "$line" ]; then
      info "Verifying checksum ..."
      expected="$(printf '%s' "$line" | awk '{print $1}')"
      if command -v sha256sum >/dev/null 2>&1; then
        actual="$(sha256sum "$tmp/$3" | awk '{print $1}')"
      else
        actual="$(shasum -a 256 "$tmp/$3" | awk '{print $1}')"
      fi
      [ "$expected" = "$actual" ] || err "Checksum mismatch for $3 (expected $expected, got $actual)."
    else
      warn "No checksum entry for $3 in SHA256SUMS; skipping verification."
    fi
  else
    warn "No SHA256SUMS published for $1 $2; skipping checksum verification."
  fi
}

fetch "$REPO" "$VERSION" "$asset"

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

# The ui tool (eskiu-lang/ui): one binary, bin/ui, next to eskiuc.
if [ "$WITH_UI" = "1" ]; then
  fetch "$UI_REPO" "$UI_VERSION" "ui-$plat.tar.gz"
  tar -xzf "$tmp/ui-$plat.tar.gz" -C "$prefix"
  [ -x "$prefix/bin/ui" ] || err "Install failed: $prefix/bin/ui not found after extraction."
  info "Installed ui to $prefix/bin/ui"
fi

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
if [ "$WITH_UI" = "1" ]; then say "      and:  ui new myapp && cd myapp && ui serve"; fi
