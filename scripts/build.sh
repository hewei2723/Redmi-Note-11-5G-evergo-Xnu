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
[ -n "$NDK" ] && [ -d "$NDK" ] || die "Android NDK not found; pass --ndk or set ANDROID_NDK_ROOT"

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
export PATH="$TREE/.bin:$TREE/Mu_Basecore/BaseTools/BinWrappers/PosixLike:$CLANG_DIR:/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin"

mkdir -p "$TREE/.bin" "$TREE/Conf"
if [ ! -x "$TREE/.bin/build" ]; then
  say "generating EDK2 base tools"
  python3 "$EDK_TOOLS_PATH/Source/C/bin" 2>/dev/null || true
  ( cd "$TREE/Mu_Basecore" && make -C BaseTools -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" )
fi

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
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
echo "sha256: $(shasum -a 256 "$OUT" | cut -d' ' -f1)"
echo
echo "The FD is not yet a boot image. Package it with scripts/make-boot-image.sh"
echo "and verify it on hardware as described in README.md."
