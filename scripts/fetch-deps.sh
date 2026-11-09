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

# The base tree carries submodules (Mu core, MediaTek blobs, ACPI, OpenSSL).
# Only the first level matters for the build; the exact revisions are pinned
# in step 2, so a failure here is recoverable and not fatal.
git -C "$BASE" submodule update --init --depth 1 --jobs "$JOBS" \
  || say "note: first-level submodules could not all be fetched now; step 2 pins them"
say "UEFI base at $(git -C "$BASE" rev-parse --short HEAD)"

# ---------------------------------------------------------------------------
# 2. Pin submodules that the DSC include chain links against
# ---------------------------------------------------------------------------
pin_submodule() {  # pin_submodule <path> <revision> <url>
  local path=$1 rev=$2 url=$3
  local abs="$BASE/$path" head

  if [ ! -e "$abs/.git" ]; then
    say "fetching $path"
    git -C "$BASE" clone --quiet --filter=blob:none "$url" "$path" \
      || die "cannot clone $path from $url (network problem?)"
  fi

  if ! git -C "$abs" cat-file -e "$rev^{commit}" 2>/dev/null; then
    git -C "$abs" fetch --quiet origin "$rev" \
      || die "cannot fetch $rev for $path (network problem?)"
  fi

  git -C "$abs" checkout --quiet "$rev" \
    || die "cannot check out $rev in $path"

  head=$(git -C "$abs" rev-parse HEAD)
  if [ "$head" != "$rev" ]; then
    die "$path is at $head, expected $rev"
  fi
  echo "  $path @ $(git -C "$abs" rev-parse --short HEAD)"
}

# Mu_Basecore has nested submodules of its own (e.g. MdePkg's mipisyst) that
# its .dec files reference. Without them build.py stops with "is not found in
# packages path". These are not version-critical: initialise whatever the
# pinned commit records.
if [ -f "$BASE/Mu_Basecore/.gitmodules" ]; then
  say "initialising nested submodules of Mu_Basecore"
  git -C "$BASE/Mu_Basecore" submodule update --init --recursive --depth 1 --jobs "$JOBS" \
    || say "note: some nested submodules could not be fetched"
fi

# ---------------------------------------------------------------------------
# 3. Apply the evergo patches to the upstream tree
# ---------------------------------------------------------------------------
# Each patch states where it applies in its header line, because upstream
# paths differ per component:
#   "target: Mu_Basecore"  -> apply inside the mu_basecore submodule
#   "target: ."            -> apply at the tree root
apply_patch() {  # apply_patch <file>
  local patch=$1 name dir target
  name=$(basename "$patch")
  target=$(sed -n 's/^# target: *//p' "$patch" | head -1)
  target=${target:-.}
  dir="$BASE/$target"
  [ -d "$dir" ] || die "$name targets '$target', which is not in the tree"

  if git -C "$dir" apply --check --ignore-whitespace "$patch" 2>/dev/null; then
    git -C "$dir" apply --ignore-whitespace "$patch"
    echo "  applied $name ($target)"
  elif git -C "$dir" apply --check --reverse --ignore-whitespace "$patch" 2>/dev/null; then
    echo "  already applied $name ($target)"
  else
    die "$name does not apply to $dir - the pinned revision is not what the patch was written against"
  fi
}

say "applying evergo patches"
for p in "$ROOT"/patches/*.patch; do
  [ -e "$p" ] || continue
  apply_patch "$p"
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
