#!/usr/bin/env bash
# Shared helpers. Sourced by the other scripts; not executable on its own.
#
# Everything here exists because the project is built on at least three host
# families (macOS, Linux, WSL2) and must behave identically on all of them.

# sha256_file <path>
#   macOS ships `shasum`, most Linux distributions ship `sha256sum`, and WSL2
#   has both. Pick whichever exists instead of assuming one.
sha256_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | cut -d' ' -f1
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | cut -d' ' -f1
  elif command -v openssl >/dev/null 2>&1; then
    openssl dgst -sha256 "$1" | sed 's/.*= *//'
  else
    echo "no sha256 tool found (sha256sum, shasum or openssl)" >&2
    return 1
  fi
}

# nproc_safe
#   `nproc` is Linux-only; `sysctl -n hw.ncpu` is macOS-only. Neither is
#   guaranteed under WSL2's default image.
nproc_safe() {
  if command -v nproc >/dev/null 2>&1; then
    nproc
  elif command -v sysctl >/dev/null 2>&1 && sysctl -n hw.ncpu >/dev/null 2>&1; then
    sysctl -n hw.ncpu
  else
    echo 4
  fi
}
