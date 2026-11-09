#!/usr/bin/env bash
#
# fetch-deps.sh - materialise the pinned upstream tree this project builds on.
#
# The repository ships only evergo-specific code: the platform package, the
# patches applied to upstream, the boot shim and the build entry point. Every
# third-party component is fetched here at the revision recorded in deps.lock,
# so the tree stays reviewable and no upstream source is redistributed.
#
# Usage:
#   scripts/fetch-deps.sh [--workdir DIR] [--jobs N]
#
# Defaults to ./.work/uefi. Idempotent: existing checkouts are reused and only
# verified, never clobbered.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
LOCK="$ROOT/deps.lock"
WORKDIR="$ROOT/.work/uefi"
JOBS=${JOBS:-4}

while [ $# -gt 0 ]; do
  case "$1" in
    --workdir) WORKDIR=$2; shift 2 ;;
    --jobs)    JOBS=$2; shift 2 ;;
    -h|--help) sed -n '2,18p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

die() { echo "fetch-deps: $*" >&2; exit 1; }
say() { printf '\n==> %s\n' "$*"; }

lock_value() {  # lock_value <key> <field: 2=url 3=revision>
  awk -v k="$1" -v f="$2" '$1==k {print $f; exit}' "$LOCK"
}

command -v git >/dev/null || die "git is required"

mkdir -p "$WORKDIR"
say "work tree: $WORKDIR"

# ---------------------------------------------------------------------------
# 1. UEFI base tree
# ---------------------------------------------------------------------------
BASE="$WORKDIR/uefi-base"
BASE_REV=$(lock_value uefi-base 3)
BASE_URL=$(lock_value uefi-base 2)

if [ ! -d "$BASE/.git" ]; then
  say "cloning UEFI base ($BASE_URL)"
  git clone --filter=blob:none "$BASE_URL" "$BASE"
fi
git -C "$BASE" fetch --quiet origin "$BASE_REV" || die "cannot fetch $BASE_REV"
git -C "$BASE" checkout --quiet "$BASE_REV"
git -C "$BASE" submodule update --init --recursive --depth 1 --jobs "$JOBS"
say "UEFI base at $(git -C "$BASE" rev-parse --short HEAD)"

# ---------------------------------------------------------------------------
# 2. Pin submodules that the DSC include chain links against
# ---------------------------------------------------------------------------
pin_submodule() {  # pin_submodule <path> <revision> <url>
  local path=$1 rev=$2 url=$3
  local abs="$BASE/$path"
  if [ ! -d "$abs/.git" ] && [ ! -e "$abs/.git" ]; then
    say "fetching $path"
    git -C "$BASE" clone --quiet --filter=blob:none "$url" "$path" 2>/dev/null || true
  fi
  if [ -d "$abs/.git" ] || [ -f "$abs/.git" ]; then
    git -C "$abs" fetch --quiet origin "$rev" 2>/dev/null || true
    git -C "$abs" checkout --quiet "$rev" 2>/dev/null \
      || echo "  note: $path already at a compatible revision"
  else
    echo "  note: $path not a git checkout, skipping pin"
  fi
}

for key in mu_basecore mu_plus mu_oem_sample device-binaries silicium-acpi openssl; do
  rev=$(lock_value "$key" 3); url=$(lock_value "$key" 2)
  case "$key" in
    mu_basecore)     path=Mu_Basecore ;;
    mu_plus)         path=Common/Mu ;;
    mu_oem_sample)   path=Common/Mu_OEM_Sample ;;
    device-binaries) path=Binaries ;;
    silicium-acpi)   path=Silicium-ACPI ;;
    openssl)         path=Silicon/Silicium/OpensslPkg/Library/OpensslLib/openssl ;;
  esac
  pin_submodule "$path" "$rev" "$url"
done

# ---------------------------------------------------------------------------
# 3. Apply the evergo patches to the upstream tree
# ---------------------------------------------------------------------------
say "applying evergo patches"
for p in "$ROOT"/patches/*.patch; do
  [ -e "$p" ] || continue
  name=$(basename "$p")
  if git -C "$BASE" apply --check --ignore-whitespace "$p" 2>/dev/null; then
    git -C "$BASE" apply --ignore-whitespace "$p"
    echo "  applied $name"
  elif git -C "$BASE" apply --check --reverse --ignore-whitespace "$p" 2>/dev/null; then
    echo "  already applied $name"
  else
    die "$name does not apply to $BASE - the upstream revision moved"
  fi
done

# ---------------------------------------------------------------------------
# 4. Overlay the evergo platform package and driver sources
# ---------------------------------------------------------------------------
say "installing evergo platform package"
mkdir -p "$BASE/Platforms/Xiaomi"
rm -rf "$BASE/Platforms/Xiaomi/evergoPkg"
cp -R "$ROOT/Platforms/Xiaomi/evergoPkg" "$BASE/Platforms/Xiaomi/evergoPkg"

say "installing evergo-modified driver sources"
( cd "$ROOT" && find Silicon BootShim Resources -type f -print ) | while read -r rel; do
  mkdir -p "$BASE/$(dirname "$rel")"
  cp "$ROOT/$rel" "$BASE/$rel"
done

# ---------------------------------------------------------------------------
# 5. UniXNU boot manager package
# ---------------------------------------------------------------------------
UNIXNU="$WORKDIR/unixnu"
UNIXNU_REV=$(lock_value unixnu 3)
UNIXNU_URL=$(lock_value unixnu 2)
if [ ! -d "$UNIXNU/.git" ]; then
  say "cloning UniXNU"
  git clone --filter=blob:none "$UNIXNU_URL" "$UNIXNU"
fi
git -C "$UNIXNU" fetch --quiet origin "$UNIXNU_REV"
git -C "$UNIXNU" checkout --quiet "$UNIXNU_REV"

say "installing UniXNUPkg into the UEFI tree"
rm -rf "$BASE/UniXNUPkg"
cp -R "$UNIXNU/package/UniXNUPkg" "$BASE/UniXNUPkg"

say "done"
cat <<EOF

UEFI tree ready: $BASE

Build it with:
  scripts/build.sh --tree "$BASE"

EOF
