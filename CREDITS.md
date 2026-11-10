# Credits and third-party notices

This port is derived work. Nothing here would exist without the projects below,
and their licences and authorship apply to the parts they contributed.

## Upstream projects

### Project Silicium — Mu-Silicium

* **Repository:** https://github.com/Project-Silicium/Mu-Silicium
* **Pinned revision:** `8221489a628aa47daa0776322ee17ce8dc7a9981` (see `deps.lock`)
* **Licence:** BSD-2-Clause-Patent (see `LICENSE`)
* **What we use:** the whole UEFI tree. `Silicon/Silicium/SiliciumPkg`
  (SEC, DXE glue, `SimpleFbDxe`, `KeypadDxe`, `BootManagerLib`),
  `Silicon/MediaTek/MediaTekPkg` (MT6833 support, GPIO, PMIC, PMIC wrapper),
  the EDK2 build integration, and the `Platforms/` layout convention.
* **What we changed for evergo:** see `Silicon/` and `BootShim/` in this
  repository — those are modified upstream files, kept whole for review. The
  device package `Platforms/Xiaomi/evergoPkg` is written for this port.
* **Note:** the platform package began from Silicium's `lancelotPkg` /
  `MT6789Pkg` layout and was reworked for evergo's memory map, framebuffer and
  boot policy. Where a file is still inherited, its original copyright headers
  are intact.

### UniXNU

* **Repository:** https://github.com/UniXNU/UniXNU
* **Pinned revision:** `d3de950a23a54d454d6ac21d8deceaa3f4ad0e73` (see `deps.lock`)
* **Licence:** BSD-2-Clause-Patent
* **What we use:** `package/UniXNUPkg` — the boot menu application, the boot
  entry model, the config parser and the DarwinBoot handoff. Fetched at build
  time by `scripts/fetch-deps.sh`; **not** vendored in this repository, and not
  modified. The verified build ran upstream code as-is.
* **What we changed:** only the firmware-side integration — how the boot
  manager finds, loads and starts `UniXNU.efi` from the firmware volume
  (`BootManagerLib/BootManager.c`), and which modules the FV contains.

### hackios-pencilpen1 — XNU loader and payload container design

* **Used by:** `Silicon/Silicium/SiliciumPkg/Library/BootManagerLib/XnuLoader.c`
  and `XnuLoader.h`, and the `XNUP` container format those files describe.
* **What we use:** the design of the stage-0 XNU loader: an `XNUP` container
  holding `loader.efi`, the layout of that container, and the beacon-based
  diagnostics. The code was ported and then changed for evergo: this device has
  no boot shim on the hot-jump path, so the container is expected in the
  existing no-map DTB reservation at `0xB5400000` instead of at the address the
  original used, and a missing `loader.efi` returns to BDS instead of
  cold-resetting.
* **Provenance:** the source comments in both files name the origin explicitly
  so the ported parts stay distinguishable from evergo-specific work.
* **Licence:** BSD-2-Clause-Patent, consistent with the rest of the UEFI tree.
  If you redistribute this repository, keep the provenance comments intact.

### Microsoft Mu (mu_basecore, mu_plus, mu_oem_sample)

* **Repositories:** https://github.com/microsoft/mu_basecore,
  https://github.com/microsoft/mu_plus,
  https://github.com/microsoft/mu_oem_sample
* **Pinned revisions:** `3c9c7b2e211e62d2a954ad760a36fe05adc43aa3`,
  `b8d46c71610318e36a661819e1e3c0b8bdda413a`,
  `b70b4b7bb02c43bf9db2172dfb3ed0457e2361d6`
* **Licence:** BSD-2-Clause-Patent
* **What we use:** the EDK2 core the firmware is built on.
* **What we changed:** `patches/0001-evergo-mu_basecore.patch` — 19 files. The
  substantive changes are GIC redistributor wake-up and spurious-interrupt
  handling, generic timer fixes for a hot-jump entry, a memory-permission and
  exception-handler rework for AArch64, and BDS/DXE progress beacons used for
  diagnosis. The patch applies to the pinned commit with
  `git apply --ignore-whitespace` (upstream files use CRLF).

### Project Silicium — Device-Binaries and Silicium-ACPI

* **Repositories:** https://github.com/Project-Silicium/Device-Binaries,
  https://github.com/Project-Silicium/Silicium-ACPI
* **Pinned revisions:** `a1501e0c2e642d6ac0d8f52c85af2a67b75a7799`,
  `1c8bd63a7ffcaecf3c8764a6b01a11e053457543`
* **What we use:** prebuilt MT6833 blobs (DDR training, PLL and PMIC
  sequences) and the ACPI table generators. Both are fetched as dependencies,
  not committed here.

### OpenSSL

* **Repository:** https://github.com/openssl/openssl
* **Pinned revision:** `67b5686b4419b4cb8caa502711c41815f5279751`
* **Licence:** Apache-2.0 (OpenSSL 3.x)
* **What we use:** linked by `OpensslPkg` for the authenticated-variable path.
  Fetched as a submodule, not committed.

### Android boot image format

* **Source:** Android Open Source Project, `system/tools/mkbootimg`
* **Licence:** Apache-2.0
* **What we use:** the boot v1 header layout, re-implemented in
  `Resources/Scripts/mkbootimg.py` and `scripts/make-boot-image.sh`. No AOSP
  code is copied.

## Device-specific material

### `Resources/DTBs/evergo.dtb`

The device tree blob extracted unmodified from the stock firmware for this
model. It is the device's own hardware description; it is not authored here.
Vendor DTB licensing is generally unclear, so it is included because the
firmware cannot describe the device without it, and it is marked as such in the
README. If you redistribute this repository commercially, check the position
for your jurisdiction.

### Measured hardware values

Several constants in `MemoryMapLib`, `CamelliaBootMemoryDxe` and the shim were
measured on a physical device (GIC and PMIC bases, display subsystem, panel
geometry, framebuffer base, reserved DRAM windows). They are facts about the
hardware, recorded in comments next to their use.

## Licence layering

Two different licences apply, and they are compatible because the works are
separate:

| Part | Licence | Why |
| --- | --- | --- |
| UEFI firmware, platform package, shim, docs | BSD-2-Clause-Patent (`LICENSE`) | inherited from the UEFI upstreams |
| `stage1/evergo_stage1.c` | GPL-2.0 | it is a Linux kernel module; the kernel module interface requires it |

The kernel module links against the Linux kernel, not against the firmware. It
ships its own `MODULE_LICENSE("GPL")` and is not combined into the BSD-licensed
firmware binary, so the two do not impose terms on each other.

## Not included

This repository deliberately does **not** contain:

* any Apple code, kernel, kext, firmware or binary,
* `DarwinBoot` or any other prebuilt XNU boot component,
* a prebuilt hot-jump kernel module binary (the source is in `stage1/`)
* stock firmware images or partition dumps,
* third-party source trees (fetched at pinned revisions instead).

## How to attribute this work

If you build on this port, attribute Project Silicium and UniXNU as the
upstreams, and note the evergo adaptation separately:

> UEFI and XNU boot chain for the Redmi Note 11 5G (evergo, MT6833), derived
> from Project Silicium's Mu-Silicium and integrating UniXNU as the boot
> manager.
