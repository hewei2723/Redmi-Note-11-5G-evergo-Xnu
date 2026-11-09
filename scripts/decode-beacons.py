#!/usr/bin/env python3
"""evergo beacon decoder.

The firmware writes progress markers into two RAM pages that survive a warm
reset on this device:

  beacon page  0x4811FC00  32 x u64, written by SEC, the DXE core, BDS and the
                           XNU loader hook. Slot 0 is the magic 0x53473142
                           ("B1GS" little-endian) followed by the last stage.
  TRC ring     0x4811F800  2 KiB record ring (magic 0x31435254 "TRC1").

Both pages live inside the reserved ramoops window the device's own DTB
declares, which is why the Android kernel does not zero them before the module
reads them back. Nothing here writes to the device.

Usage:
    scripts/decode-beacons.py --beacon FILE [--trace FILE] [--json]

FILE is a raw dump produced on the phone, for example:

    su -c 'dd if=/dev/mem bs=1 skip=$((0x4811fc00)) count=256 2>/dev/null' > beacon.bin
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

BEACON_MAGIC = 0x53473142  # "B1GS"
TRC_MAGIC = 0x31435254  # "TRC1"

# SEC writes the stage into slot 0 low half and a step id into slot 1.
STAGES = {
    0x01: "SEC entry",
    0x02: "SEC: DTB parsed",
    0x03: "SEC: memory map built",
    0x04: "SEC: decompressing firmware volume",
    0x05: "SEC: FV decompressed",
    0x06: "SEC: entering DXE",
    0xA0: "DXE core entry",
    0xA1: "DXE: arch protocols installed",
    0xA2: "DXE: dispatch complete",
    0xA3: "BDS: after console",
    0xA4: "BDS: boot option selected",
    0xA5: "BDS: image started",
    0xA6: "BDS: unable to boot (idle)",
    0xAD: "XNU hook: no payload container found",
    0xAE: "XNU hook: container present, loader loading",
    0xAF: "XNU hook: loader returned",
    0xE6: "SEC: DecompressFirstFv failed",
}


def describe_stage(value: int) -> str:
    return STAGES.get(value & 0xFF, f"unknown stage {value & 0xFF:#04x}")


def decode_beacon(blob: bytes) -> dict:
    if len(blob) < 16:
        return {"valid": False, "reason": f"short read ({len(blob)} bytes)"}
    magic, stage = struct.unpack_from("<II", blob, 0)
    if magic != BEACON_MAGIC:
        return {
            "valid": False,
            "reason": "magic mismatch - page was overwritten or never written",
            "magic": f"{magic:#010x}",
            "expected": f"{BEACON_MAGIC:#010x}",
        }
    slots = list(struct.unpack_from("<32Q", blob, 0)) if len(blob) >= 256 else []
    return {
        "valid": True,
        "stage": stage,
        "stage_text": describe_stage(stage),
        "slots": slots,
        # The loader hook sets bit 63 of slot 17 once XnuLoaderMain returns.
        "xnu_loader_ran": bool(len(slots) > 17 and slots[17] >> 63),
        "input_devices": slots[18] if len(slots) > 18 else None,
        "textin_handles": slots[21] if len(slots) > 21 else None,
        "console_in": slots[22] if len(slots) > 22 else None,
    }


def decode_trace(blob: bytes) -> dict:
    if len(blob) < 16:
        return {"valid": False, "reason": f"short read ({len(blob)} bytes)"}
    magic, count, head = struct.unpack_from("<III", blob, 0)
    if magic != TRC_MAGIC:
        return {"valid": False, "reason": "TRC1 magic missing"}
    records = []
    for index in range(min(count, 15)):
        off = 0x80 + index * 0x80
        if off + 32 > len(blob):
            break
        tag, detail, a, b = struct.unpack_from("<IIQQ", blob, off)
        records.append(
            {"index": index, "tag": f"{tag:#06x}", "detail": detail, "a": a, "b": b}
        )
    return {"valid": True, "count": count, "head": head, "records": records}


def main() -> int:
    ap = argparse.ArgumentParser(description="decode evergo firmware beacons")
    ap.add_argument("--beacon", type=Path, help="raw 0x4811FC00 dump")
    ap.add_argument("--trace", type=Path, help="raw 0x4811F800 dump")
    ap.add_argument("--json", action="store_true", help="emit machine-readable output")
    args = ap.parse_args()

    if not args.beacon and not args.trace:
        ap.error("give --beacon and/or --trace")

    result: dict = {}
    if args.beacon:
        result["beacon"] = decode_beacon(args.beacon.read_bytes())
    if args.trace:
        result["trace"] = decode_trace(args.trace.read_bytes())

    if args.json:
        print(json.dumps(result, indent=2))
        return 0

    beacon = result.get("beacon")
    if beacon:
        print("== beacon page ==")
        if not beacon["valid"]:
            print(f"  INVALID: {beacon['reason']}")
            if "magic" in beacon:
                print(f"  read {beacon['magic']}, expected {beacon['expected']}")
            print("  The page was overwritten or the firmware never wrote it.")
            print("  This is not evidence that the firmware did not run.")
        else:
            print(f"  stage      {beacon['stage']:#04x}  {beacon['stage_text']}")
            print(f"  XNU loader ran: {beacon['xnu_loader_ran']}")
            if beacon.get("textin_handles") is not None:
                print(f"  TextIn handles: {beacon['textin_handles']}")
            if beacon.get("input_devices") is not None:
                print(f"  input devices with a device path: {beacon['input_devices']}")
            if beacon.get("console_in") is not None:
                conin = beacon["console_in"]
                print(f"  gST->ConIn: {conin:#x}" if conin else "  gST->ConIn: NULL")

    trace = result.get("trace")
    if trace:
        print("== TRC ring ==")
        if not trace["valid"]:
            print(f"  INVALID: {trace['reason']}")
        else:
            print(f"  records {trace['count']}, head {trace['head']}")
            for rec in trace["records"]:
                print(
                    f"  [{rec['index']:02d}] {rec['tag']} "
                    f"detail={rec['detail']:#x} a={rec['a']:#x} b={rec['b']:#x}"
                )
    return 0


if __name__ == "__main__":
    sys.exit(main())
