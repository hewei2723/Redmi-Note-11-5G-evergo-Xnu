#!/usr/bin/env bash
#
# build.sh - build the evergo UEFI firmware volume (SILICIUM_UEFI.fd).
#
# The FD must be linked for the hot-jump load address. A plain `build` with the
# DSC defaults produces a cold-boot image that faults as soon as the MMU is on,
# so this script always passes the hot-jump geometry explicitly:
#
#   FD_BASE  0xB0000000   where evergo_stage1 copies the FD before jumping
#   FD_SIZE  0x200000     2 MiB, matching the DTB no-map slot
#
# Usage:
#   scripts/build.sh --tree DIR [--out FILE] [--ndk DIR] [--target DEBUG]
#
# --tree  the UEFI tree prepared by scripts/fetch-deps.sh (contains Platforms/)
# --out   destination for the built FD (default: ./out/SILICIUM_UEFI.fd)
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=scripts/lib.sh
. "$ROOT/scripts/lib.sh"
TREE=""
OUT="$ROOT/out/SILICIUM_UEFI.fd"
NDK=${ANDROID_NDK_ROOT:-${ANDROID_NDK_HOME:-}}
TARGET=DEBUG
FD_BASE=0xB0000000
FD_SIZE=0x200000
FD_BLOCKS=0x200

while [ $# -gt 0 ]; do
  case "$1" in
    --tree)   TREE=$2; shift 2 ;;
    --out)    OUT=$2; shift 2 ;;
    --ndk)    NDK=$2; shift 2 ;;
    --target) TARGET=$2; shift 2 ;;
    --fd-base) FD_BASE=$2; shift 2 ;;
    --fd-size) FD_SIZE=$2; shift 2 ;;
    -h|--help) sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

die() { echo "build: $*" >&2; exit 1; }
say() { printf '\n==> %s\n' "$*"; }

[ -n "$TREE" ] || die "--tree is required (run scripts/fetch-deps.sh first)"
[ -d "$TREE" ] || die "no such tree: $TREE"
[ -f "$TREE/Platforms/Xiaomi/evergoPkg/evergo.dsc" ] || die "$TREE is not a prepared evergo tree"

# ---------------------------------------------------------------------------
# Toolchain
# ---------------------------------------------------------------------------
if [ -z "$NDK" ]; then
  for candidate in \
    "$HOME/Library/Android/sdk/ndk"/* \
    "$HOME/Android/Sdk/ndk"/* \
    /opt/android-ndk* ; do
    [ -d "$candidate/toolchains/llvm/prebuilt" ] && NDK=$candidate
  done
fi
if [ -z "$NDK" ] || [ ! -d "$NDK" ]; then
  cat >&2 <<'MSG'
build: Android NDK not found.

The EDK2 CLANGPDB toolchain needs clang, lld and llvm-objcopy from the Android
NDK (r26 or newer; this project was verified with r30). A distribution clang
will not do: the build passes -target aarch64-unknown-windows-msvc, which needs
the NDK's lld.

Install it and point this script at it:

  macOS    brew install --cask android-ndk
           scripts/build.sh --tree .work/uefi --ndk "$HOME/Library/Android/sdk/ndk/30.0.16248370"

  Linux    wget https://dl.google.com/android/repository/android-ndk-r30-linux.zip
           unzip android-ndk-r30-linux.zip
           scripts/build.sh --tree .work/uefi --ndk "$PWD/android-ndk-r30"

  WSL2     same as Linux; keep the NDK inside the Linux filesystem
           (e.g. ~/android-ndk-r30), not under /mnt/c, or the build is very slow.

Or set ANDROID_NDK_ROOT once and omit --ndk.
MSG
  exit 1
fi

case "$(uname -s)-$(uname -m)" in
  Darwin-arm64)   HOST=darwin-x86_64 ;;   # NDK ships an x86_64 host toolchain
  Darwin-x86_64)  HOST=darwin-x86_64 ;;
  Linux-x86_64)   HOST=linux-x86_64 ;;
  Linux-aarch64)  HOST=linux-x86_64 ;;
  *) die "unsupported host: $(uname -s)-$(uname -m)" ;;
esac
CLANG_DIR="$NDK/toolchains/llvm/prebuilt/$HOST/bin"
[ -d "$CLANG_DIR" ] || die "no LLVM toolchain at $CLANG_DIR"

say "tree:      $TREE"
say "toolchain: $CLANG_DIR"

# ---------------------------------------------------------------------------
# EDK2 environment
# ---------------------------------------------------------------------------
export WORKSPACE="$TREE"
export EDK_TOOLS_PATH="$TREE/Mu_Basecore/BaseTools"
export CONF_PATH="$TREE/Conf"
export PACKAGES_PATH="$TREE/Platforms/Xiaomi:$TREE/UniXNUPkg:$TREE/Common/Mu:$TREE/Common/Mu_OEM_Sample:$TREE/Mu_Basecore:$TREE/Silicon/MediaTek:$TREE/Silicon/Silicium:$TREE/Silicium-ACPI:$TREE/Silicium-ACPI/Platforms/Xiaomi:$TREE/Silicium-ACPI/Silicon/MediaTek"
export CLANG_BIN="$CLANG_DIR/" CLANG_HOST_BIN="/usr/bin/"
# EDK2's BaseTools wrappers shell out to a `python` command that many systems
# do not provide. Point them at the interpreter running this script instead.
PYTHON_CMD=$(command -v python3 || command -v python || true)
[ -n "$PYTHON_CMD" ] || die "python3 is required"
export PYTHON_COMMAND="$PYTHON_CMD"
export PATH="$TREE/.bin:$TREE/Mu_Basecore/BaseTools/BinWrappers/PosixLike:$CLANG_DIR:/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin"

mkdir -p "$TREE/.bin" "$TREE/Conf"
if [ ! -x "$TREE/Mu_Basecore/BaseTools/BinWrappers/PosixLike/build" ]; then
  die "$TREE/Mu_Basecore/BaseTools is incomplete; re-run scripts/fetch-deps.sh"
fi
# EDK2's C base tools need libuuid headers (uuid/uuid.h) and, on some
# distributions, a C++ compiler for VfrCompile. Check before invoking make so
# the failure names the missing package instead of a wall of compiler output.
missing=""
if [ ! -f /usr/include/uuid/uuid.h ] && ! echo '#include <uuid/uuid.h>' | \
     ${CC:-cc} -E - >/dev/null 2>&1; then
  missing="libuuid development headers"
fi
if ! command -v g++ >/dev/null 2>&1 && ! command -v c++ >/dev/null 2>&1; then
  missing="$missing${missing:+, }a C++ compiler"
fi
if [ -n "$missing" ] && [ ! -x "$TREE/Mu_Basecore/BaseTools/Source/C/bin/GenFw" ]; then
  cat >&2 <<MSG
build: missing host packages needed to build the EDK2 base tools: $missing

  Debian/Ubuntu/WSL2:  sudo apt install -y build-essential uuid-dev
  Fedora:              sudo dnf install -y gcc gcc-c++ libuuid-devel
  Arch:                sudo pacman -S --needed base-devel util-linux
  macOS:               xcode-select --install   (libuuid is part of the SDK)

MSG
  exit 1
fi

if [ ! -x "$TREE/Mu_Basecore/BaseTools/Source/C/bin/GenFw" ]; then
  say "building EDK2 base tools (first run only)"
  # BaseTools builds a handful of optional helpers (BrotliCompress, VfrCompile,
  # ...). A failure in one of those is not fatal for this platform, which needs
  # GenFw/GenFfs/GenFv/GenSec and the Python drivers. Judge by the tools the
  # build actually uses rather than by make's exit status.
  ( cd "$TREE/Mu_Basecore" && make -C BaseTools -j"$(nproc_safe)" ) \
    || echo "  note: BaseTools reported errors; checking the required tools"
  for tool in GenFw GenFfs GenFv GenSec; do
    [ -x "$TREE/Mu_Basecore/BaseTools/Source/C/bin/$tool" ] \
      || die "required EDK2 tool $tool was not built"
  done
  say "EDK2 base tools ready"
fi

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
# EDK2 reads Conf/{target,tools_def,build_rule}.txt. They are generated from
# the BaseTools templates on first use; without them build.py stops with
# "File/directory not found in workspace: Conf/tools_def.txt".
if [ ! -f "$TREE/Conf/tools_def.txt" ]; then
  say "creating EDK2 Conf/ from BaseTools templates"
  for f in target tools_def build_rule; do
    tpl="$TREE/Mu_Basecore/BaseTools/Conf/$f.template"
    if [ -f "$tpl" ]; then
      cp "$tpl" "$TREE/Conf/$f.txt"
    else
      die "missing template: $tpl"
    fi
  done
fi

say "building evergoPkg ($TARGET, CLANGPDB, AARCH64)"
( cd "$TREE" && build \
    -a AARCH64 -t CLANGPDB -b "$TARGET" \
    -p Platforms/Xiaomi/evergoPkg/evergo.dsc \
    -D FD_BASE="$FD_BASE" -D FD_SIZE="$FD_SIZE" -D FD_BLOCKS="$FD_BLOCKS" \
    -D DEVICE_MODEL=0 -D ENABLE_SECUREBOOT=0 )

BUILT="$TREE/Build/evergoPkg/${TARGET}_CLANGPDB/FV/SILICIUM_UEFI.fd"
[ -f "$BUILT" ] || die "build finished but $BUILT is missing"

SIZE=$(wc -c < "$BUILT" | tr -d ' ')
[ "$SIZE" = "$((FD_SIZE))" ] || die "unexpected FD size $SIZE (want $((FD_SIZE)))"

mkdir -p "$(dirname "$OUT")"
cp "$BUILT" "$OUT"
say "FD written to $OUT"
echo "sha256: $(sha256_file "$OUT")"
echo
echo "The FD is not yet a boot image. Package it with scripts/make-boot-image.sh"
echo "and verify it on hardware as described in README.md."
