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
# The hot-jump path reproduces the verified menu run (2026-11-09) exactly:
#
#   1. push the FD and the stage1 module (default: the repo's bundled .ko)
#   2. kptr_restrict=0 so /proc/kallsyms shows real symbol addresses
#   3. wake the screen and dismiss the keyguard (KEYCODE_WAKEUP + MENU):
#      the DPU layer state at jump time decides whether the panel shows the
#      firmware output cleanly or as 8x-folded bands
#   4. mode=11: publish the framebuffer handoff and GATE on its geometry
#      (byte_pitch=4352, pages=2550). A wrong pitch is exactly the
#      "vertically repeated pictures" display failure; refuse to jump.
#   5. mode=5: copy the FD (and an optional XNUP payload) and jump
#   6. --read-back: dump the beacon page and decode it. /dev/mem does not
#      exist on this kernel (CONFIG_DEVMEM is off), so the beacons are read
#      back with the module's own mode=7 dump.
#
# Usage:
#   scripts/deploy.sh --fd FILE    --serial SERIAL --yes [--read-back]
#                     [--ko FILE] [--payload FILE]
#   scripts/deploy.sh --image FILE --serial SERIAL --yes --flash
#                     [--dry-run] [--read-back]
#
#   --fd FILE      out/SILICIUM_UEFI.fd from scripts/build.sh   (hot jump)
#   --image FILE   out/evergo-boot.img from make-boot-image.sh  (--flash only)
#   --serial       adb serial (see `adb devices`)
#   --ko FILE      stage1 module to use (default: magisk/evergo-jump/evergo_stage1.ko)
#   --payload FILE optional XNUP container copied to the payload window 0xB5400000
#   --flash        write boot_b instead of the in-RAM hot jump  (RISKY)
#   --read-back    after the device returns, dump the beacon page and decode it
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=scripts/lib.sh
. "$ROOT/scripts/lib.sh"
FD=""; IMAGE=""; SERIAL=${ANDROID_SERIAL:-}; CONFIRM=0; FLASH=0; DRY=0; READBACK=0
KO="$ROOT/magisk/evergo-jump/evergo_stage1.ko"
PAYLOAD=""

while [ $# -gt 0 ]; do
  case "$1" in
    --fd) FD=$2; shift 2 ;;
    --image) IMAGE=$2; shift 2 ;;
    --serial) SERIAL=$2; shift 2 ;;
    --ko) KO=$2; shift 2 ;;
    --payload) PAYLOAD=$2; shift 2 ;;
    --yes) CONFIRM=1; shift ;;
    --flash) FLASH=1; shift ;;
    --dry-run) DRY=1; shift ;;
    --read-back) READBACK=1; shift ;;
    -h|--help) sed -n '2,42p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
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

say "pushing stage1 module"
[ -f "$KO" ] || die "stage1 module not found: $KO
  Download it from the repository's Releases page, or build it from stage1/
  (see stage1/README.md - the 'struct module init offset pitfall' section).
  If you use a downloaded .ko, verify it with:
    llvm-readelf -r evergo_stage1.ko | grep -A3 this_module
  the init relocation must be at r_offset 0x158 for this phone's kernel."
"$ADB" -s "$SERIAL" push "$KO" /data/local/tmp/evergo_stage1.ko >/dev/null
KO_SHA=$(sha256_file "$KO")
DEV_KO_SHA=$("$ADB" -s "$SERIAL" shell "sha256sum /data/local/tmp/evergo_stage1.ko" 2>/dev/null | tr -d '\r' | cut -d' ' -f1)
[ "$KO_SHA" = "$DEV_KO_SHA" ] || die "module push corrupted (host $KO_SHA, device $DEV_KO_SHA)"
"$ADB" -s "$SERIAL" shell "su -c 'cp /data/local/tmp/evergo_stage1.ko /data/adb/evergo_stage1.ko && chmod 644 /data/adb/evergo_stage1.ko'"
echo "  sha256 $KO_SHA"

PAYLOAD_ARGS=""
if [ -n "$PAYLOAD" ]; then
  say "pushing XNUP payload"
  "$ADB" -s "$SERIAL" push "$PAYLOAD" /data/local/tmp/payload-evergo.bin >/dev/null
  PL_SHA=$(sha256_file "$PAYLOAD")
  DEV_PL_SHA=$("$ADB" -s "$SERIAL" shell "sha256sum /data/local/tmp/payload-evergo.bin" 2>/dev/null | tr -d '\r' | cut -d' ' -f1)
  [ "$PL_SHA" = "$DEV_PL_SHA" ] || die "payload push corrupted"
  "$ADB" -s "$SERIAL" shell "su -c 'cp /data/local/tmp/payload-evergo.bin /data/adb/payload-evergo.bin && chmod 644 /data/adb/payload-evergo.bin'"
  echo "  sha256 $PL_SHA"
  PAYLOAD_ARGS="p_payload_path=/data/adb/payload-evergo.bin p_payload_base=0xb5400000"
fi

# ---------------------------------------------------------------------------
# Preconditions for a clean display: symbol access and a stable screen state.
# ---------------------------------------------------------------------------
say "preparing screen state and symbol access"
"$ADB" -s "$SERIAL" shell "su -c 'echo 0 > /proc/sys/kernel/kptr_restrict'" >/dev/null 2>&1 \
  || die "cannot set kernel.kptr_restrict to 0 (need root)"
"$ADB" -s "$SERIAL" shell input keyevent KEYCODE_WAKEUP
sleep 1
"$ADB" -s "$SERIAL" shell input keyevent 82
sleep 1
echo "  screen awake, keyguard dismissed"

# ---------------------------------------------------------------------------
# Framebuffer handoff gate (mode=11): the panel only shows the firmware output
# cleanly when the handoff geometry is exactly 1088x2400 / pitch 4352 / 2550
# contiguous pages. A wrong pitch is the "vertically repeated pictures" bug.
# ---------------------------------------------------------------------------
say "publishing framebuffer handoff (mode=11) and gating on geometry"
M11=$("$ADB" -s "$SERIAL" shell 'su -c "
  rmmod evergo_stage1 2>/dev/null
  insmod /data/adb/evergo_stage1.ko mode=11
  dmesg | grep -a \"mode=11\" | tail -2
  rmmod evergo_stage1 2>/dev/null
"' 2>&1 | tr -d '\r' | grep -a "mode=11" | tail -2)
[ -n "$M11" ] || die "mode=11 handoff publish produced no output - is the screen on? not jumping"
echo "$M11" | sed 's/^/  /'
# The geometry line reports "byte_pitch=4352"; the handoff line reports
# "pages=2550". Both must be present, or the panel will fold the frame.
echo "$M11" | grep -q "byte_pitch=4352" \
  || die "geometry gate FAILED: byte_pitch is not 4352 - not jumping (wake the screen and retry)"
echo "$M11" | grep -q "pages=2550" \
  || die "geometry gate FAILED: pages is not 2550 - not jumping"
echo "  geometry OK (1088x2400, pitch 4352, 2550 pages)"

# ---------------------------------------------------------------------------
# 2. Hot jump through the kernel module
# ---------------------------------------------------------------------------
say "hot-jumping (no partition is written)"
# The adb connection is EXPECTED to die mid-command when the jump fires
# (the phone leaves Android), so do not let that abort the script.
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
  taskset 1 insmod /data/adb/evergo_stage1.ko mode=5 confirm=1 stop=1 wdt_reset=0 \
    fd_path=/data/adb/evergo-hotjump.fd \
    p_fd_base=0xb0000000 p_jump_pa=0xb0000000 \
    p_smp_send_stop=0x\$S p_filp_open=0x\$F p_kernel_read=0x\$R \
    p_flush_dcache_area=0x\$FA p_flush_pa=0x0 p_flush_len=0x0 '"$PAYLOAD_ARGS"' \
    || echo JUMP_FAILED
"' 2>&1 | tr -d '\r' | tail -8 || true

echo "  the phone now runs the firmware; the display is the only output"

# ---------------------------------------------------------------------------
# 3. Read the beacon back once Android returns
# ---------------------------------------------------------------------------
if [ "$READBACK" = 1 ]; then
  say "waiting for Android to come back"
  state=""
  for _ in $(seq 1 60); do
    state=$("$ADB" -s "$SERIAL" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')
    [ "$state" = "1" ] && break
    sleep 4
  done
  [ "$state" = "1" ] || die "device did not return to Android within 4 minutes"

  say "reading beacons"
  # /dev/mem does not exist on this kernel (CONFIG_DEVMEM is off), so read the
  # beacon page with the module's own mode=7 dump. The Magisk module may have
  # restored a stale .ko at boot, so re-push ours first.
  "$ADB" -s "$SERIAL" push "$KO" /data/local/tmp/evergo_stage1.ko >/dev/null
  "$ADB" -s "$SERIAL" shell "su -c 'cp /data/local/tmp/evergo_stage1.ko /data/adb/evergo_stage1.ko && chmod 644 /data/adb/evergo_stage1.ko && echo 0 > /proc/sys/kernel/kptr_restrict'" >/dev/null
  "$ADB" -s "$SERIAL" shell 'su -c "
    F=\$(awk \"\\\$3==\\\"filp_open\\\"{print \\\$1}\" /proc/kallsyms)
    W=\$(awk \"\\\$3==\\\"kernel_write\\\"{print \\\$1}\" /proc/kallsyms)
    rmmod evergo_stage1 2>/dev/null
    insmod /data/adb/evergo_stage1.ko mode=7 \
      p_marker_pa=0x4811F800 p_marker_count=16 \
      fd_path=/data/local/tmp/evergo-beacons.bin \
      p_filp_open=0x\$F p_kernel_write=0x\$W
    rmmod evergo_stage1 2>/dev/null
    chmod 644 /data/local/tmp/evergo-beacons.bin
  "' >/dev/null 2>&1
  "$ADB" -s "$SERIAL" pull /data/local/tmp/evergo-beacons.bin /tmp/evergo-beacons.bin >/dev/null 2>&1 \
    || die "could not pull the beacon dump"
  python3 - /tmp/evergo-beacons.bin <<'PY'
import struct, sys
data = open(sys.argv[1], 'rb').read()
# dump starts at 0x4811F800; the beacon page is at +0x400, the TRC ring at +0
slots = struct.unpack_from('<32Q', data, 0x400)
magic, stage = slots[0], slots[1] & 0xFF
STAGES = {0x01:"SEC entry",0x02:"SEC: DTB parsed",0x03:"SEC: memory map built",
          0x04:"SEC: decompressing FV",0x05:"SEC: FV decompressed",0x06:"SEC: entering DXE",
          0xA0:"DXE core entry",0xA1:"DXE: arch protocols installed",0xA2:"DXE: dispatch complete (menu launch point)",
          0xA3:"BDS: after console",0xA4:"BDS: boot option selected",0xA5:"BDS: image started",
          0xA6:"BDS: unable to boot",0xAD:"XNU hook: no payload",0xAE:"XNU hook: container present",
          0xAF:"XNU hook: loader returned"}
print("== beacon page ==")
if magic != 0x53473142:
    print(f"  INVALID magic {magic:#010x} - page overwritten; proves nothing either way")
else:
    print("  magic   B1GS valid")
    print(f"  stage   {stage:#04x}  {STAGES.get(stage, 'unknown')}")
    print(f"  FB pattern mask slot14 = {slots[14]:#018x}  (0x...3f3f = 6/6 bars read back)")
    print(f"  FB base used   slot15 = {slots[15]:#x}")
    print(f"  XNU loader returned (slot17 bit63): {bool(slots[17] >> 63)}")
    exc = [slots[i] for i in range(18, 24)]
    print(f"  exception slots 18-23 all zero: {all(v == 0 for v in exc)} (True = no fault)")
PY
fi
