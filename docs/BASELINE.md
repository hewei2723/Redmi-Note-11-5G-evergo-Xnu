# Verified baseline

[**中文**](BASELINE.zh-CN.md) | English

What "working" means in this repository, how it was established, and how to
check that a build is the same thing.

## The milestone

On 2026-11-09 the device displayed the **real UniXNU boot menu** — not a mock-up,
not a UEFI-drawn imitation of one:

* portrait UniXNU UI with the product mark, `version 0.1.0` and
  `built-in configuration`
* four entries: `Return to firmware`, `Restart`, `Power off`, `Tools`
* a running countdown (`Booting Return to firmware in 8 s` → `7 s`)
* status line showing `No input device`, `no boot entry found`,
  `drivers 0 loaded / 0 failed  kits 0  acpi 0`

Evidence: screen recording of the device, plus the firmware's own progress
beacons showing SEC, DXE dispatch and BDS `AfterConsole` completed.

## What it proves, and what it does not

Proves:

* the hot-jump path reaches UEFI on this device,
* SEC builds a memory map that survives with the MMU on,
* DXE dispatches the driver set and BDS runs its `AfterConsole` policy,
* the firmware volume publishes `UniXNU.efi` and the boot manager loads and
  starts it,
* the GOP framebuffer path reaches the panel at the native geometry.

Does **not** prove:

* that XNU executes. No kernel entry, no `Darwin Kernel Version`, no kernel
  console output was ever observed.
* that menu input works. `No input device` is the observed state.
* that a boot entry exists. `no boot entry found` is the observed state.

## The exact build

```text
FD sha256   e4c674f60f9f854fbc4fabe893b8b503b3ec07876c4eb18042e8c975949c61ae
FD size     2097152 (0x200000)
FD base     0xB0000000
target      DEBUG, CLANGPDB, AARCH64
```

Source state at that build, as recorded at the time:

```text
Silicon/Silicium/SiliciumPkg/Library/BootManagerLib/BootManager.c
  368d605005c27d5b27173bc41ea321fef4f01d4c2b1c5de70d27d1d878091df1
Platforms/Xiaomi/evergoPkg/evergo.dsc
  cb9194afd3522b12de301f1ad2636b14391afbc21d4d5919aef8035a3cc4228d
Platforms/Xiaomi/evergoPkg/Include/DXE.inc
  5e8b516fd5bd8d2b0043035d2a5f00e8767c77c0b099e87846d0d35f3d9df008
```

This repository is that source state. Everything developed after it — a
standalone keypad probe driver, input-chain beacons, a SoC keypad register
rewrite, a beacon-page relocation, a boot-manager skip switch, an input override
in the UniXNU menu — was experimental, was never confirmed on hardware, and is
**not** in this tree. See "What was removed" below.

## How the source state was recovered

The experimental work was not committed incrementally, so the baseline could not
be taken from git. It was reconstructed and then proven by rebuilding:

1. The device's own milestone directory recorded SHA-256 hashes of the three
   files above at the moment the working FD was produced. Those hashes are the
   target.
2. Every edit after that moment was replayed in reverse from the development
   session log, file by file, until each reconstructed file hashed to its
   recorded value.
3. The rebuild was then compared against the archived success FD, module by
   module.

Result: **56 of 57 firmware-volume modules are byte-identical** to the archived
success build. The one exception is `PcdDxe`, which differs only in its 8-byte
PDB signature (`RSDS…`). Rebuilding unchanged sources produces the same 8-byte
difference, so it is build noise, not a behavioural change.

Module inventory of the baseline (57 modules) can be regenerated from any build
with:

```bash
python3 - <<'PY'
import struct, uuid
from pathlib import Path
data = Path(".work/uefi/Build/evergoPkg/DEBUG_CLANGPDB/FV/FVMAIN.Fv").read_bytes()
base = data.find(b"_FVH") - 40
ext = struct.unpack_from("<H", data, base + 0x34)[0]
hdr = struct.unpack_from("<H", data, base + 0x30)[0]
off = base + max(hdr, ext + 0x14)
while data[off:off+4] == b"\xff"*4:
    off += 4
n = 0
while off + 24 <= len(data):
    name = data[off:off+16]
    if name == b"\xff"*16:
        break
    size = int.from_bytes(data[off+0x14:off+0x17], "little")
    if size == 0xFFFFFF:
        size = struct.unpack_from("<Q", data, off + 0x18)[0]
    print(f"{uuid.UUID(bytes_le=name)}  {size:#x}")
    off = (off + size + 7) & ~7
    n += 1
print(f"{n} modules")
PY
```

## What was removed relative to the experimental tree

Each of these was added after the verified build and never confirmed on
hardware. They are listed so nobody re-introduces them by accident while
believing they were part of the working version.

| Removed | Why |
| --- | --- |
| `KeypadInputTestDxe` driver and its DSC/FDF entries | a standalone probe page; never displayed correctly |
| `EVERGO_SKIP_BOOTMGR_FOR_INPUT_TEST` branch in `BootManager.c` | bypassed both the menu and the XNU hook |
| TextIn polling loop in the BDS hook | replaced the menu with a test screen |
| input-chain beacons in KeypadDxe / KeypadDeviceDxe / GpioDxe / PmicWrapperDxe / MT6358PmicDxe | wrote into a page that the Android kernel overwrites |
| TRC dispatch probes in the same drivers | same |
| beacon page relocation `0x4811FC00` → `0x48122000` | the new page was cleared on every boot |
| `MenuTextIn()` override in `UniXNUPkg/.../Menu.c` | vendored package was modified; the verified build ran upstream code |
| `KeypadDeviceDxe` depex changed to require GPIO+PMIC protocols | changed dispatch order relative to the verified build |
| SoC keypad register rewrite in `KeypadDeviceLib.c` | never produced a key event |
| `Keypad` region added to `MemoryMapLib` | changed `.rdata` layout of every module linking that library |

## Reproducing the comparison

`scripts/compare-baseline.py` compares a build against a baseline FVMAIN and
classifies each module:

```bash
# decompresses the FD for you
scripts/compare-baseline.py --fd out/SILICIUM_UEFI.fd --baseline baseline-FVMAIN.Fv
```

| Class | Meaning |
| --- | --- |
| identical | `.text` matches byte for byte |
| layout-only | differences are confined to ADRP/ADD/MOV immediates that address relocated data |
| code | instruction fields differ beyond address fixups — a real behavioural change |

Measured on this project:

| Build | Result |
| --- | --- |
| macOS, same path length, same clang | 57 identical, 0 layout-only, 0 code |
| macOS, different path length, same clang | 46 identical, 11 layout-only, 0 code |
| Linux aarch64, distro clang 18.1.8 | 6 identical, 0 layout-only, 51 code |

**The third row is not a regression.** A different clang version emits
different instructions for identical source. Cross-toolchain comparison cannot
prove equivalence in either direction, so the tool reports it as "code differs"
with an explicit warning rather than pretending it is a fault.

What the tool is genuinely good for: after you change a source file, compare
your new build against a build of the same tree on the same machine with the
same toolchain. Then a "code" finding means you changed behaviour, and a
"layout-only" finding means you only changed where a string landed.

The mechanism: the compiler embeds each module's PDB path in `.rdata`, so a
different path length resizes that string and shifts everything after it,
which changes the immediates that address it. The code is the same; only the
addresses of the embedded strings move. That is why the same-length build is
byte-identical and the different-length build is not — and why a *code* class
finding is the one that matters.

## Keeping the baseline reachable

The reconstruction is preserved as a tagged branch in the development checkout,
not in this repository (this repository *is* that state). If you fork and start
experimenting, keep a tag at the commit you received and re-run the module
comparison after every change that touches SEC, BDS or the memory map.
