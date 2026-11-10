#!/usr/bin/env bash
#
# deploy.sh - run the firmware on the phone.
#
# THIS SCRIPT TOUCHES THE DEVICE. It refuses to run unless you pass --yes.
#
# Two paths, and they take different inputs:
#
#   hot jump (default)   --fd FILE
#       The evergo_stage1 kernel module copies the raw 2 MiB FD to reserved
#       DRAM and branches into it. No partition is written; a failed attempt is
#       recovered by power-cycling.
#
#   flash (--flash)      --image FILE
#       Writes a boot image to boot_b. The phone then boots the firmware
#       through LK instead of Android, and a bad image leaves it unable to
#       start on its own.
#
# Usage:
#   scripts/deploy.sh --fd FILE    --serial SERIAL --yes [--read-back]
#   scripts/deploy.sh --image FILE --serial SERIAL --yes --flash
#                     [--dry-run] [--read-back]
#
#   --fd FILE      out/SILICIUM_UEFI.fd from scripts/build.sh   (hot jump)
#   --image FILE   out/evergo-boot.img from make-boot-image.sh  (--flash only)
#   --serial       adb serial (see `adb devices`)
#   --flash        write boot_b instead of the in-RAM hot jump  (RISKY)
#   --read-back    after the device returns, dump the beacon page and decode it
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=scripts/lib.sh
. "$ROOT/scripts/lib.sh"
FD=""; IMAGE=""; SERIAL=${ANDROID_SERIAL:-}; CONFIRM=0; FLASH=0; DRY=0; READBACK=0
BEACON_PA=0x4811fc00
TRC_PA=0x4811f800

while [ $# -gt 0 ]; do
  case "$1" in
    --fd) FD=$2; shift 2 ;;
    --image) IMAGE=$2; shift 2 ;;
    --serial) SERIAL=$2; shift 2 ;;
    --yes) CONFIRM=1; shift ;;
    --flash) FLASH=1; shift ;;
    --dry-run) DRY=1; shift ;;
    --read-back) READBACK=1; shift ;;
    -h|--help) sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

die() { echo "deploy: $*" >&2; exit 1; }
say() { printf '\n==> %s\n' "$*"; }

if [ "$FLASH" = 1 ]; then
  [ -n "$IMAGE" ] && [ -f "$IMAGE" ] || die "--flash needs --image FILE (a boot image from make-boot-image.sh)"
else
  [ -n "$FD" ] && [ -f "$FD" ] || die "--fd FILE is required (the 2 MiB FD from scripts/build.sh)"
  size=$(wc -c < "$FD" | tr -d ' ')
  [ "$size" = "2097152" ] || die "$FD is $size bytes, expected 2097152 - that is not a raw FD"
fi
[ -n "$SERIAL" ] || die "--serial is required (refusing to guess which device)"
ADB=${ADB:-adb}
command -v "$ADB" >/dev/null || die "adb not found"

say "device $SERIAL"
"$ADB" -s "$SERIAL" get-state >/dev/null 2>&1 || die "device $SERIAL is not attached"
MODEL=$("$ADB" -s "$SERIAL" shell getprop ro.product.device 2>/dev/null | tr -d '\r')
[ "$MODEL" = "evergo" ] || die "ro.product.device is '$MODEL', expected 'evergo' - wrong phone?"
echo "  model: $MODEL"

if [ "$FLASH" = 1 ]; then
  cat <<'WARN'

  ------------------------------------------------------------------
  WARNING: --flash writes the boot partition.

  A bad boot image on boot_b means the phone no longer starts Android on
  its own. Recovery needs a working `fastboot flash boot_b` with a known
  good image, which in turn needs an unlocked bootloader. Only continue if
  you have that image on this machine right now.
  ------------------------------------------------------------------

WARN
  [ "$CONFIRM" = 1 ] || die "refusing to flash without --yes"
fi

if [ "$DRY" = 1 ]; then
  if [ "$FLASH" = 1 ]; then
    say "dry run: would flash $IMAGE ($(wc -c < "$IMAGE" | tr -d ' ') bytes) to boot_b"
  else
    say "dry run: would hot-jump $FD ($(wc -c < "$FD" | tr -d ' ') bytes) via evergo_stage1"
  fi
  exit 0
fi

[ "$CONFIRM" = 1 ] || die "this script changes device state; re-run with --yes"

if [ "$FLASH" = 1 ]; then
  # -------------------------------------------------------------------------
  # Flash path: LK unpacks the boot image, so it takes the wrapped artifact.
  # -------------------------------------------------------------------------
  say "pushing boot image"
  "$ADB" -s "$SERIAL" push "$IMAGE" /data/local/tmp/evergo-boot.img >/dev/null
  HOST_SHA=$(sha256_file "$IMAGE")
  DEV_SHA=$("$ADB" -s "$SERIAL" shell "sha256sum /data/local/tmp/evergo-boot.img" 2>/dev/null | tr -d '\r' | cut -d' ' -f1)
  [ "$HOST_SHA" = "$DEV_SHA" ] || die "push corrupted (host $HOST_SHA, device $DEV_SHA)"
  echo "  sha256 $HOST_SHA"

  say "flashing boot_b"
  "$ADB" -s "$SERIAL" reboot bootloader
  sleep 5
  fastboot flash boot_b "$IMAGE"
  fastboot reboot
  exit 0
fi

# ---------------------------------------------------------------------------
# Hot-jump path: the module copies the raw FD itself, so push that.
# ---------------------------------------------------------------------------
say "pushing FD"
"$ADB" -s "$SERIAL" push "$FD" /data/local/tmp/evergo-hotjump.fd >/dev/null
HOST_SHA=$(sha256_file "$FD")
DEV_SHA=$("$ADB" -s "$SERIAL" shell "sha256sum /data/local/tmp/evergo-hotjump.fd" 2>/dev/null | tr -d '\r' | cut -d' ' -f1)
[ "$HOST_SHA" = "$DEV_SHA" ] || die "push corrupted (host $HOST_SHA, device $DEV_SHA)"
echo "  sha256 $HOST_SHA"

# ---------------------------------------------------------------------------
# 2. Hot jump through the kernel module
# ---------------------------------------------------------------------------
say "hot-jumping (no partition is written)"
if ! "$ADB" -s "$SERIAL" shell 'su -c "test -f /data/adb/evergo_stage1.ko"' >/dev/null 2>&1; then
  die "evergo_stage1.ko is not installed on the device; build it from stage1/ (see stage1/README.md)"
fi

# The module needs the addresses of a few kernel symbols. They change per boot,
# so they are read on the device rather than passed from here.
"$ADB" -s "$SERIAL" shell 'su -c "
  cp /data/local/tmp/evergo-hotjump.fd /data/adb/evergo-hotjump.fd
  S=\$(awk \"\\\$3==\\\"smp_send_stop\\\"{print \\\$1}\" /proc/kallsyms)
  F=\$(awk \"\\\$3==\\\"filp_open\\\"{print \\\$1}\" /proc/kallsyms)
  R=\$(awk \"\\\$3==\\\"kernel_read\\\"{print \\\$1}\" /proc/kallsyms)
  FA=\$(awk \"\\\$3==\\\"__flush_dcache_area\\\"{print \\\$1}\" /proc/kallsyms)
  if [ -z \"\$S\" ] || [ -z \"\$F\" ] || [ -z \"\$R\" ]; then
    echo \"JUMP_FAILED: cannot read symbol addresses from /proc/kallsyms\"
    echo \"  (kernel.kptr_restrict is probably 1; set it to 0 as root)\"
    exit 1
  fi
  taskset 1 insmod /data/adb/evergo_stage1.ko mode=5 stop=1 \
    fd_path=/data/adb/evergo-hotjump.fd \
    p_fd_base=0xb0000000 p_jump_pa=0xb0000000 \
    p_smp_send_stop=0x\$S p_filp_open=0x\$F p_kernel_read=0x\$R \
    p_flush_dcache_area=0x\$FA p_flush_pa=0x0 p_flush_len=0x0 \
    || echo JUMP_FAILED
"' 2>&1 | tr -d '\r' | tail -8

echo "  the phone now runs the firmware; the display is the only output"

# ---------------------------------------------------------------------------
# 3. Read the beacon back once Android returns
# ---------------------------------------------------------------------------
if [ "$READBACK" = 1 ]; then
  say "waiting for Android to come back"
  for _ in $(seq 1 60); do
    state=$("$ADB" -s "$SERIAL" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')
    [ "$state" = "1" ] && break
    sleep 4
  done
  [ "$state" = "1" ] || die "device did not return to Android within 4 minutes"

  say "reading beacons"
  "$ADB" -s "$SERIAL" shell "su -c 'dd if=/dev/mem bs=256 skip=\$((0x4811fc00/256)) count=1 2>/dev/null'" > /tmp/evergo-beacon.bin
  "$ADB" -s "$SERIAL" shell "su -c 'dd if=/dev/mem bs=2048 skip=\$((0x4811f800/2048)) count=1 2>/dev/null'" > /tmp/evergo-trace.bin
  python3 "$ROOT/scripts/decode-beacons.py" --beacon /tmp/evergo-beacon.bin --trace /tmp/evergo-trace.bin
fi
