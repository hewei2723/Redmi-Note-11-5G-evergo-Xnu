#!/usr/bin/env bash
#
# make-boot-image.sh - package the UEFI FD into an Android boot image that LK
# will accept, so the firmware can be reached without touching any partition.
#
# Layout produced (Android boot v1 header taken from the stock boot_b image):
#
#   kernel region = gzip(BootShim.bin || FD || optional XNUP payload)
#   ramdisk       = evergo DTB
#
# BootShim runs at the address LK loads the kernel to, copies the FD to
# 0xB0000000 (and the payload, if present, to 0xB5400000), records the DTB
# pointer the firmware expects at 0xA40007F8, then branches into the FD.
#
# Usage:
#   scripts/make-boot-image.sh --fd FILE --shim FILE --dtb FILE --out FILE
#                              [--payload FILE] [--header-template FILE]
#
# Every argument is explicit on purpose: this script never guesses a device
# layout. --header-template should be the 2048-byte boot v1 header of a stock
# boot image for the exact model, because the load addresses live in it.
set -euo pipefail

FD=""; SHIM=""; DTB=""; OUT=""; PAYLOAD=""; TEMPLATE=""

while [ $# -gt 0 ]; do
  case "$1" in
    --fd) FD=$2; shift 2 ;;
    --shim) SHIM=$2; shift 2 ;;
    --dtb) DTB=$2; shift 2 ;;
    --out) OUT=$2; shift 2 ;;
    --payload) PAYLOAD=$2; shift 2 ;;
    --header-template) TEMPLATE=$2; shift 2 ;;
    -h|--help) sed -n '2,24p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

die() { echo "make-boot-image: $*" >&2; exit 1; }

[ -n "$FD" ] && [ -f "$FD" ] || die "--fd FILE is required"
[ -n "$SHIM" ] && [ -f "$SHIM" ] || die "--shim FILE is required"
[ -n "$DTB" ] && [ -f "$DTB" ] || die "--dtb FILE is required"
[ -n "$OUT" ] || die "--out FILE is required"

python3 - "$FD" "$SHIM" "$DTB" "$OUT" "$PAYLOAD" "$TEMPLATE" <<'PY'
import gzip, io, struct, sys
from pathlib import Path

fd_path, shim_path, dtb_path, out_path, payload_path, template_path = sys.argv[1:7]

fd = Path(fd_path).read_bytes()
shim = Path(shim_path).read_bytes()
dtb = Path(dtb_path).read_bytes()
payload = Path(payload_path).read_bytes() if payload_path else b""

FD_BASE = 0xB0000000
FD_SIZE = 0x200000
PAYLOAD_BASE = 0xB5400000

if len(fd) != FD_SIZE:
    sys.exit(f"FD is {len(fd):#x} bytes, expected {FD_SIZE:#x}")
if len(payload) > 0x1000000:
    sys.exit("payload larger than the reserved window")

kernel = shim + fd + payload
kernel_gz = gzip.compress(kernel, compresslevel=9, mtime=0)

# Android boot v1 header: 8-byte magic, then 10 little-endian u32 fields.
# Page size and load addresses come from the template when one is supplied.
if template_path:
    hdr = bytearray(Path(template_path).read_bytes()[:2048])
    if len(hdr) != 2048:
        sys.exit("header template must be at least 2048 bytes")
else:
    hdr = bytearray(2048)

hdr[0:8] = b"ANDROID!"
page_size = struct.unpack_from("<I", hdr, 36)[0] or 2048
kernel_size = len(kernel_gz)
ramdisk_size = len(dtb)

struct.pack_into("<I", hdr, 8, kernel_size)
struct.pack_into("<I", hdr, 16, ramdisk_size)
struct.pack_into("<I", hdr, 24, 0)          # second size
struct.pack_into("<I", hdr, 32, 0)          # recovery dtbo size

def pad(blob, page):
    rem = len(blob) % page
    return blob if rem == 0 else blob + b"\0" * (page - rem)

image = bytes(hdr) + pad(kernel_gz, page_size) + pad(dtb, page_size)
Path(out_path).write_bytes(image)

# Self-check: unpack what we just wrote and compare against the inputs.
check = Path(out_path).read_bytes()
assert check[:8] == b"ANDROID!"
klen = struct.unpack_from("<I", check, 8)[0]
rlen = struct.unpack_from("<I", check, 16)[0]
assert klen == kernel_size and rlen == ramdisk_size
restored = gzip.decompress(check[page_size:page_size + klen])
assert restored == kernel, "kernel region did not round-trip"
assert check[page_size + ((klen + page_size - 1) // page_size) * page_size:
             page_size + ((klen + page_size - 1) // page_size) * page_size + rlen] == dtb

print(f"image      {out_path}")
print(f"page size  {page_size}")
print(f"kernel     {kernel_size} bytes gzipped (shim {len(shim)} + fd {len(fd)} + payload {len(payload)})")
print(f"ramdisk    {rlen} bytes (evergo DTB)")
print(f"total      {len(image)} bytes")
print(f"load       shim at LK kernel address, FD -> {FD_BASE:#x}, payload -> {PAYLOAD_BASE:#x}")
PY
