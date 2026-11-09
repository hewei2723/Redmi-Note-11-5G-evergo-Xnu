#!/usr/bin/env bash
#
# deploy.sh - push an image to the phone and (optionally) start it.
#
# THIS SCRIPT TOUCHES THE DEVICE. It refuses to run unless you pass --yes.
#
# The default path never flashes a partition:
#   * push the boot image to /data/local/tmp
#   * hand it to the evergo_stage1 kernel module, which copies it into the
#     reserved hot-jump window and resets the SoC
#
# That keeps boot_b intact, so a failed experiment is recovered by simply
# rebooting. Flashing (--flash) is offered for completeness and is the one
# operation that can leave the phone unbootable.
#
# Usage:
#   scripts/deploy.sh --image FILE --serial SERIAL --yes
#                     [--flash] [--dry-run] [--read-back]
#
#   --image FILE   boot image from scripts/make-boot-image.sh
#   --serial       adb serial (see `adb devices`)
#   --flash        write boot_b instead of the in-RAM hot jump  (RISKY)
#   --read-back    after the device returns, dump the beacon page and decode it
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=scripts/lib.sh
. "$ROOT/scripts/lib.sh"
IMAGE=""; SERIAL=${ANDROID_SERIAL:-}; CONFIRM=0; FLASH=0; DRY=0; READBACK=0
BEACON_PA=0x4811fc00
TRC_PA=0x4811f800

while [ $# -gt 0 ]; do
  case "$1" in
    --image) IMAGE=$2; shift 2 ;;
    --serial) SERIAL=$2; shift 2 ;;
    --yes) CONFIRM=1; shift ;;
    --flash) FLASH=1; shift ;;
    --dry-run) DRY=1; shift ;;
    --read-back) READBACK=1; shift ;;
    -h|--help) sed -n '2,26p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

die() { echo "deploy: $*" >&2; exit 1; }
say() { printf '\n==> %s\n' "$*"; }

[ -n "$IMAGE" ] && [ -f "$IMAGE" ] || die "--image FILE is required"
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
  say "dry run: would push $IMAGE ($(wc -c < "$IMAGE" | tr -d ' ') bytes)"
  exit 0
fi

[ "$CONFIRM" = 1 ] || die "this script changes device state; re-run with --yes"

# ---------------------------------------------------------------------------
# 1. Push
# ---------------------------------------------------------------------------
say "pushing image"
"$ADB" -s "$SERIAL" push "$IMAGE" /data/local/tmp/evergo-boot.img >/dev/null
HOST_SHA=$(sha256_file "$IMAGE")
DEV_SHA=$("$ADB" -s "$SERIAL" shell "sha256sum /data/local/tmp/evergo-boot.img" 2>/dev/null | tr -d '\r' | cut -d' ' -f1)
[ "$HOST_SHA" = "$DEV_SHA" ] || die "push corrupted (host $HOST_SHA, device $DEV_SHA)"
echo "  sha256 $HOST_SHA"

if [ "$FLASH" = 1 ]; then
  say "flashing boot_b"
  "$ADB" -s "$SERIAL" reboot bootloader
  sleep 5
  fastboot flash boot_b "$IMAGE"
  fastboot reboot
  exit 0
fi

# ---------------------------------------------------------------------------
# 2. Hot jump through the kernel module
# ---------------------------------------------------------------------------
say "hot-jumping (no partition is written)"
if ! "$ADB" -s "$SERIAL" shell 'su -c "test -f /data/adb/evergo_stage1.ko"' >/dev/null 2>&1; then
  die "evergo_stage1.ko is not installed on the device; see docs/RUNBOOK.md"
fi

"$ADB" -s "$SERIAL" shell 'su -c "
  insmod /data/adb/evergo_stage1.ko mode=5 image=/data/local/tmp/evergo-boot.img || echo JUMP_FAILED
"' 2>&1 | tr -d '\r' | tail -5

echo "  the phone resets into the firmware; the display is the only output"

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
