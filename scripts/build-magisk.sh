#!/usr/bin/env bash
#
# build-magisk.sh — assemble the Evergo UEFI Jump Magisk module.
#
# The module bundles three binaries that are not committed to git:
#   - EvergoJump.apk      built from magisk/apk-src/
#   - evergo_stage1.ko    taken from the stage1 release (or built from stage1/)
#   - fd/evergo-hotjump.fd the firmware, built by scripts/build.sh
#
# This script produces magisk/evergo-jump-v1.0.zip, ready to install in Magisk.
#
# Usage:
#   scripts/build-magisk.sh --fd out/SILICIUM_UEFI.fd --ko /path/to/evergo_stage1.ko
#
# Both --fd and --ko are required: the module is useless without them, and
# silently building a zip that is missing one would only fail on the phone.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=scripts/lib.sh
. "$ROOT/scripts/lib.sh"

FD=""; KO=""
while [ $# -gt 0 ]; do
  case "$1" in
    --fd) FD=$2; shift 2 ;;
    --ko) KO=$2; shift 2 ;;
    -h|--help) sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

die() { echo "build-magisk: $*" >&2; exit 1; }
say() { printf '\n==> %s\n' "$*"; }

[ -n "$FD" ] && [ -f "$FD" ] || die "--fd FILE is required (out/SILICIUM_UEFI.fd)"
[ -n "$KO" ] && [ -f "$KO" ] || die "--ko FILE is required (evergo_stage1.ko)"

size=$(wc -c < "$FD" | tr -d ' ')
[ "$size" = "2097152" ] || die "$FD is $size bytes, expected 2097152 - not a raw FD"

MOD="$ROOT/magisk/evergo-jump"
[ -d "$MOD" ] || die "module skeleton missing at $MOD"

# --- 1. build the APK ------------------------------------------------------

say "building the companion APK"
SDK=${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Library/Android/sdk}}
BT=$(ls -d "$SDK"/build-tools/* 2>/dev/null | sort -V | tail -1)
PLATFORM=$(ls -d "$SDK"/platforms/android-* 2>/dev/null | sort -V | tail -1)/android.jar
[ -n "$BT" ] && [ -x "$BT/aapt2" ] || die "Android build-tools not found under $SDK"
[ -f "$PLATFORM" ] || die "android.jar not found under $SDK/platforms"

JAVA_HOME=${JAVA_HOME:-$(/usr/libexec/java_home 2>/dev/null || echo "")}
export PATH="${JAVA_HOME:+$JAVA_HOME/bin:}$PATH"
command -v javac >/dev/null || die "javac not found; install a JDK"

SRC="$ROOT/magisk/apk-src"
B="$SRC/build"
rm -rf "$B" && mkdir -p "$B/res" "$B/gen" "$B/classes"

"$BT/aapt2" compile --dir "$SRC/res" -o "$B/res.zip"
"$BT/aapt2" link -o "$B/base.apk" -I "$PLATFORM" \
  --manifest "$SRC/AndroidManifest.xml" --java "$B/gen" "$B/res.zip"
javac -source 8 -target 8 -bootclasspath "$PLATFORM" -classpath "$PLATFORM" \
  -d "$B/classes" "$B/gen/com/evergo/jump/R.java" \
  "$SRC/src/com/evergo/jump/MainActivity.java" 2>/dev/null
"$BT/d8" --lib "$PLATFORM" --min-api 21 --output "$B" \
  $(find "$B/classes" -name '*.class')
cp "$B/base.apk" "$B/unsigned.apk"
( cd "$B" && zip -q unsigned.apk classes.dex )
"$BT/zipalign" -f 4 "$B/unsigned.apk" "$B/aligned.apk"

# A throwaway signing key. The APK only ever runs on the user's own phone, so
# a self-signed key is correct; it is not published and not reused elsewhere.
KS="$B/evergo.keystore"
if [ ! -f "$KS" ]; then
  keytool -genkeypair -keystore "$KS" -alias evergo -keyalg RSA -keysize 2048 \
    -validity 10000 -storepass evergo -keypass evergo \
    -dname "CN=evergo-jump, OU=port, O=evergo, C=NA" >/dev/null 2>&1
fi
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:evergo --key-pass pass:evergo \
  --ks-key-alias evergo --out "$B/EvergoJump.apk" "$B/aligned.apk"
"$BT/apksigner" verify "$B/EvergoJump.apk" || die "APK signature did not verify"
echo "  APK: $(wc -c < "$B/EvergoJump.apk" | tr -d ' ') bytes"

# --- 2. assemble the module ------------------------------------------------

say "assembling the module"
cp "$B/EvergoJump.apk" "$MOD/EvergoJump.apk"
cp "$KO" "$MOD/evergo_stage1.ko"
mkdir -p "$MOD/fd"
cp "$FD" "$MOD/fd/evergo-hotjump.fd"
chmod +x "$MOD/system/bin/evergo-jump" "$MOD/customize.sh" \
         "$MOD/META-INF/com/google/android/update-binary"

OUT="$ROOT/magisk/evergo-jump-v1.0.zip"
rm -f "$OUT"
( cd "$MOD" && zip -rq "$OUT" . -x '*.DS_Store' )

say "done"
cat <<EOF

Module: $OUT
  $(wc -c < "$OUT" | tr -d ' ') bytes

  fd sha256   $(sha256_file "$FD")
  ko sha256   $(sha256_file "$KO")
  apk sha256  $(sha256_file "$B/EvergoJump.apk")

Install with: magisk --install-module <zip>   (or Magisk app -> Modules)
EOF
