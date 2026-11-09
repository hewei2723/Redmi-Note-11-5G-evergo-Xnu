#!/usr/bin/env bash
#
# build-shim.sh - assemble BootShim.bin for a given payload size.
#
# BootShim is position-independent AArch64 code that LK jumps into. It copies
# the UEFI FD to 0xB0000000, optionally copies an XNUP payload to 0xB5400000,
# records the DTB pointer the firmware reads at 0xA40007F8, and branches to
# the FD entry point.
#
# Usage:
#   scripts/build-shim.sh --tree DIR --out FILE [--payload-size BYTES]
#                         [--ndk DIR] [--no-kernel-header]
#
# --payload-size  0 (default) builds a shim without payload copy support
# --no-kernel-header  skip the 64-byte ARM64 Image header LK checks
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=scripts/lib.sh
. "$ROOT/scripts/lib.sh"
TREE=""; OUT="$ROOT/out/BootShim.bin"; PSIZE=0; NDK=${ANDROID_NDK_ROOT:-}; KHEADER=1
FD_BASE=0xB0000000
FD_SIZE=0x200000
PAYLOAD_BASE=0xB5400000

while [ $# -gt 0 ]; do
  case "$1" in
    --tree) TREE=$2; shift 2 ;;
    --out) OUT=$2; shift 2 ;;
    --payload-size) PSIZE=$2; shift 2 ;;
    --ndk) NDK=$2; shift 2 ;;
    --no-kernel-header) KHEADER=0; shift ;;
    -h|--help) sed -n '2,18p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

die() { echo "build-shim: $*" >&2; exit 1; }
say() { printf '\n==> %s\n' "$*"; }

[ -n "$TREE" ] || die "--tree is required"
[ -d "$TREE/BootShim" ] || die "no BootShim directory in $TREE"
# Absolute, because make is invoked from inside the tree and relative paths
# stop resolving once the working directory changes.
TREE=$(cd "$TREE" && pwd)

if [ -z "$NDK" ]; then
  for candidate in \
    "$HOME/Library/Android/sdk/ndk"/* \
    "$HOME/Android/Sdk/ndk"/* \
    "$HOME/Android/android-sdk/ndk"/* \
    /usr/local/lib/android/sdk/ndk/* \
    /opt/android-sdk/ndk/* \
    /opt/android-ndk* \
    "$HOME"/android-ndk* \
    "$HOME"/Android/android-ndk* ; do
    if [ -d "$candidate/toolchains/llvm/prebuilt" ]; then
      NDK=$candidate
    fi
  done
fi
if [ -z "$NDK" ] || [ ! -d "$NDK" ]; then
  cat >&2 <<'MSG'
build-shim: Android NDK not found.

BootShim is assembled with the NDK's clang. Install it (r26 or newer; verified
with r30) and pass --ndk DIR, or set ANDROID_NDK_ROOT.

  macOS    brew install --cask android-ndk
  Linux    unzip android-ndk-r30-linux.zip    (from dl.google.com/android/repository)
  WSL2     as Linux; keep it under ~ rather than /mnt/c
MSG
  exit 1
fi

case "$(uname -s)-$(uname -m)" in
  Darwin-*) HOST=darwin-x86_64 ;;
  Linux-*)  HOST=linux-x86_64 ;;
  *) die "unsupported host" ;;
esac
BIN="$NDK/toolchains/llvm/prebuilt/$HOST/bin"
[ -x "$BIN/clang" ] || die "no clang at $BIN"

# Round the payload window up to 16 bytes; the shim copies whole words.
if [ "$PSIZE" != 0 ]; then
  PSIZE=$(python3 -c "n=$PSIZE; print(hex((n+15)//16*16))")
fi

say "building BootShim (payload window $PSIZE)"
( cd "$TREE/BootShim" && rm -f BootShim.elf BootShim.bin && \
  make CC="$BIN/clang" OBJCOPY="$BIN/llvm-objcopy" \
       REQUIRES_KERNEL_HEADER=$KHEADER FD_BASE=$FD_BASE FD_SIZE=$FD_SIZE \
       PAYLOAD_BASE=$PAYLOAD_BASE PAYLOAD_SIZE=$PSIZE )

SRC="$TREE/BootShim/BootShim.bin"
[ -f "$SRC" ] || die "BootShim.bin was not produced"
mkdir -p "$(dirname "$OUT")"
cp "$SRC" "$OUT"

say "BootShim written to $OUT"
echo "size:   $(wc -c < "$OUT" | tr -d ' ') bytes"
echo "sha256: $(sha256_file "$OUT")"
if [ "$KHEADER" = 1 ]; then
  python3 - "$OUT" <<'PY'
import struct, sys
data = open(sys.argv[1], "rb").read()
if data[0x38:0x3c] == b"ARM\x64":
    text_off, image_size = struct.unpack_from("<QQ", data, 8)
    print(f"kernel header: present (text_offset {text_off:#x}, image_size {image_size:#x})")
else:
    print("kernel header: MISSING - LK may reject this image")
PY
fi
