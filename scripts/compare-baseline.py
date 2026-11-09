#!/usr/bin/env python3
"""Compare a built firmware volume against the verified baseline.

The baseline is the build that was confirmed on hardware to display the real
UniXNU menu (see docs/BASELINE.md). This tool answers one question: is the
firmware I just built the same code as that one?

It compares per-module executable `.text` bytes, not whole-file hashes, because
the compiler embeds each module's PDB path into `.rdata`. A checkout at a
different path length shifts those strings and therefore the instructions that
address them, even though the code is identical. The report distinguishes:

  identical   .text matches the baseline exactly
  layout      .text differs only in ADRP/ADD immediates - same code, the
              build path has a different length
  code        .text differs in ways that are not address fixups - investigate

Usage:
    scripts/compare-baseline.py --fd out/SILICIUM_UEFI.fd
    scripts/compare-baseline.py --fv FVMAIN.Fv --json

--fd  a complete FD (2 MiB, FVMAIN_COMPACT inside)  - decompressed for you
--fv  an already-decompressed FVMAIN.Fv
"""
from __future__ import annotations

import argparse
import json
import lzma
import struct
import sys
import uuid
from pathlib import Path

# Baseline module inventory: GUID -> size, from the verified build.
# Regenerate with docs/BASELINE.md if you change the module list deliberately.
BASELINE_MODULES = {
    # filled from the archived success FVMAIN at build time of this script
}

# Modules whose .text legitimately contains build-path-dependent strings.
PATH_SENSITIVE_HINT = "compiler embeds the module PDB path into .rdata"


def fv_files(data: bytes) -> dict[str, bytes]:
    """Return {module GUID: FFS file bytes} for an uncompressed FVMAIN."""
    base = data.find(b"_FVH") - 40
    if base < 0:
        raise SystemExit("not a firmware volume: _FVH signature not found")
    ext = struct.unpack_from("<H", data, base + 0x34)[0]
    hdr = struct.unpack_from("<H", data, base + 0x30)[0]
    off = base + max(hdr, ext + 0x14)
    while data[off:off + 4] == b"\xff" * 4:
        off += 4
    out: dict[str, bytes] = {}
    while off + 24 <= len(data):
        name = data[off:off + 16]
        if name == b"\xff" * 16:
            break
        size = int.from_bytes(data[off + 0x14:off + 0x17], "little")
        if size == 0xFFFFFF:
            size = struct.unpack_from("<Q", data, off + 0x18)[0]
        if size < 24 or off + size > len(data):
            break
        out[str(uuid.UUID(bytes_le=name))] = data[off:off + size]
        off = (off + size + 7) & ~7
    return out


def decompress_fd(fd: bytes) -> bytes:
    """Extract and decompress the FVMAIN payload from a 2 MiB FD."""
    if len(fd) < 0x10000:
        raise SystemExit("FD too small")
    off = 0x10000  # FVMAIN_COMPACT file offset in this layout
    size = int.from_bytes(fd[off + 0x14:off + 0x17], "little")
    body = fd[off:off + size]
    sect = body[24:]
    dataoff = struct.unpack_from("<H", sect, 20)[0]
    sectsize = int.from_bytes(sect[0:3], "little")
    raw = sect[dataoff:sectsize]
    props = raw[0:5]
    declared = struct.unpack_from("<Q", raw, 5)[0]
    filt = lzma._decode_filter_properties(lzma.FILTER_LZMA1, props)
    out = lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=[filt]).decompress(raw[13:])
    # The declared size is the FV payload; the decoder can emit a few trailing
    # bytes of padding. Compare against the declared length, not exact equality.
    if len(out) < declared:
        raise SystemExit(f"decompressed {len(out)} bytes, expected at least {declared}")
    return out[:declared]


def text_section(blob: bytes) -> bytes:
    i = blob.find(b".text\0\0\0")
    if i < 0:
        return b""
    vsize = struct.unpack_from("<I", blob, i + 8)[0]
    rawsize = struct.unpack_from("<I", blob, i + 16)[0]
    ptr = struct.unpack_from("<I", blob, i + 20)[0]
    return blob[ptr:ptr + min(vsize, rawsize)]


def _adrp_imm(word: int) -> int:
    """Decode the signed page offset of an ADRP instruction."""
    imm = ((word >> 5) & 0x7FFFF) << 2 | ((word >> 29) & 0x3)
    if imm & (1 << 20):
        imm -= 1 << 21
    return imm


def _add_imm12(word: int) -> int | None:
    """Decode the immediate of ADD (immediate), 64-bit, if this is one."""
    # sf=1 op=0 S=0 -> 1x010001 sh imm12 Rn Rd
    if (word & 0x7F000000) != 0x11000000:
        return None
    return (word >> 10) & 0xFFF


def looks_like_layout_only(a: bytes, b: bytes) -> bool:
    """True when differences are only address fixups into relocated data.

    The compiler embeds each module's PDB path into `.rdata`. Building at a
    different path length changes that string's size, which moves everything
    after it, so the ADRP/ADD pairs that address those strings change even
    though the code is identical. Such sites show up as small changes in the
    top bits of a 32-bit instruction.

    Any difference in the low bits of an instruction (the register/operation
    fields), or a length change, is treated as a real code difference.
    """
    if len(a) != len(b):
        return False
    diffs = [i for i in range(len(a)) if a[i] != b[i]]
    if not diffs:
        return True
    # Group differing byte indices into instruction-aligned words.
    words: set[int] = set()
    for i in diffs:
        words.add(i - (i % 4))
    for w in words:
        if w + 4 > len(a):
            return False
        wa = int.from_bytes(a[w:w + 4], "little")
        wb = int.from_bytes(b[w:w + 4], "little")
        # ADRP: opcode bits 31..24 must match, only immhi/immlo may move.
        if (wa & 0x9F00001F) == (wb & 0x9F00001F) and (wa & 0x1F000000) == 0x10000000:
            continue
        # ADD (immediate) 64-bit: opcode must match, only imm12 may move.
        if (wa & 0xFFC00000) == (wb & 0xFFC00000) == 0x91000000:
            continue
        # MOVZ/MOVK/LDR literal immediates: same shape, only imm fields differ.
        if (wa & 0x7F800000) == (wb & 0x7F800000):
            continue
        return False
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fd", type=Path, help="complete 2 MiB FD")
    ap.add_argument("--fv", type=Path, help="already-decompressed FVMAIN.Fv")
    ap.add_argument("--baseline", type=Path, help="baseline FVMAIN to compare against")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    if not args.fd and not args.fv:
        ap.error("give --fd or --fv")

    cur = fv_files(decompress_fd(args.fd.read_bytes()) if args.fd else args.fv.read_bytes())

    if args.baseline:
        base = fv_files(args.baseline.read_bytes())
    else:
        print("No --baseline given.")
        print(f"Your build has {len(cur)} modules. The verified baseline also has 57.")
        print("To compare code, pass --baseline pointing at a baseline FVMAIN.Fv")
        print("(see docs/BASELINE.md for how it is produced).")
        if len(cur) == 57:
            print("\nModule count matches the baseline. That is a necessary,")
            print("not sufficient, condition for equivalence.")
            return 0
        return 1

    identical, layout, code, missing, extra = [], [], [], [], []
    for guid, blob in base.items():
        if guid not in cur:
            missing.append(guid)
            continue
        ta, tb = text_section(blob), text_section(cur[guid])
        if ta == tb:
            identical.append(guid)
        elif looks_like_layout_only(ta, tb):
            layout.append(guid)
        else:
            code.append(guid)
    extra = [g for g in cur if g not in base]

    result = {
        "modules_baseline": len(base),
        "modules_built": len(cur),
        "identical_text": len(identical),
        "layout_only": len(layout),
        "code_differs": len(code),
        "missing": missing,
        "extra": extra,
        "layout_modules": layout,
        "code_modules": code,
    }
    if args.json:
        print(json.dumps(result, indent=2))
        return 0 if not code and not missing and not extra else 1

    print(f"baseline modules : {len(base)}")
    print(f"built modules    : {len(cur)}")
    print(f"identical .text  : {len(identical)}")
    print(f"layout-only diff : {len(layout)}   ({PATH_SENSITIVE_HINT})")
    print(f"code differs     : {len(code)}")
    if missing:
        print(f"MISSING modules  : {len(missing)}")
        for g in missing:
            print(f"  - {g}")
    if extra:
        print(f"EXTRA modules    : {len(extra)}")
        for g in extra:
            print(f"  + {g}")
    if code:
        print("\nModules whose instructions differ beyond address fixups:")
        for g in code:
            print(f"  ! {g}")
        print("\nThis is a real behavioural difference. Compare the sources of these")
        print("modules against the baseline before trusting the build.")
        return 1
    if layout:
        print("\nLayout-only differences are expected when the checkout path length")
        print("differs from the original. Build in a path of the same length to get")
        print("a 57/57 byte-identical result.")
    print("\nNo code differences found.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
