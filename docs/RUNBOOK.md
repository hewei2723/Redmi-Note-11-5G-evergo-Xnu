# RUNBOOK — evergo UEFI bring-up on hardware

Step-by-step procedure for taking this repository to a physical Redmi Note 11 5G
and observing what the firmware does. Every step states what it should produce
and when to stop.

Read [`BASELINE.md`](BASELINE.md) first: it defines what "working" means, so you
do not chase a stage this project has never reached.

---

## 0. Preconditions

| Item | Check | If it fails |
| --- | --- | --- |
| Device is `evergo` | `adb shell getprop ro.product.device` → `evergo` | stop, wrong device |
| Bootloader unlocked | `fastboot oem device-info` | the hot-jump path needs it; stop |
| Root shell | `adb shell su -c id` → `uid=0` | stop |
| Stage1 module built and pushed | `/data/adb/evergo_stage1.ko` | see §1 and `stage1/README.md` |
| Stock `boot_b` image on this machine | `ls stock-boot_b.img` | required only if you will flash |

Do not proceed on a device whose `ro.product.device` is anything other than
`evergo`. The memory map and the reserved windows are specific to this model.

---

## 1. The stage1 module

The module source is in [`../stage1/`](../stage1/README.md). It performs the hot
jump: copy the 2 MiB FD to `0xB0000000`, verify the copy, clean the caches,
turn the MMU off and branch into the FD. The preloader and LK are skipped
entirely and DRAM keeps its contents, which is why a failed attempt is
recovered by power-cycling.

It takes the **raw FD**, not the boot image: the boot image is only for the
`--flash` path, where LK does the unpacking.

Build it with `stage1/Makefile` against the device kernel tree. The compiled
`.ko` is not shipped, because it must match the running kernel exactly
(version, LTO/CFI, symbol addresses).

If you cannot build it, the alternative is `deploy.sh --flash`, which replaces
`boot_b` and is destructive. See §6 before choosing that.

---

## 2. Build

```bash
scripts/fetch-deps.sh
scripts/build.sh --tree .work/uefi
scripts/build-shim.sh --tree .work/uefi
scripts/make-boot-image.sh \
  --fd   out/SILICIUM_UEFI.fd \
  --shim out/BootShim.bin \
  --dtb  .work/uefi/Resources/DTBs/evergo.dtb \
  --out  out/evergo-boot.img
```

Expected: `out/SILICIUM_UEFI.fd` is exactly 2097152 bytes; `make-boot-image.sh`
prints a round-trip check and exits 0.

Stop if the FD size differs — that means the hot-jump `-D` flags were lost and
the image is linked for the cold-boot address.

---

## 3. Baseline check before you change anything

Run the unmodified image once. This establishes that your build environment and
your device reproduce the known state, so later failures are attributable.

```bash
scripts/deploy.sh --fd out/SILICIUM_UEFI.fd --serial <serial> --yes
```

Watch the screen. Expected sequence:

1. full-screen colour bars (SEC has taken the framebuffer),
2. the UniXNU menu, countdown running,
3. either the menu waits, or the device resets back to Android.

Then read the beacons:

```bash
scripts/deploy.sh --fd out/SILICIUM_UEFI.fd --serial <serial> --yes --read-back
```

Expected: beacon magic present, stage `0xA2` or `0xA3`.

**If you do not see the menu, stop and fix that first.** Everything downstream
assumes this step works.

---

## 4. Observing a single change

One change per run. Before the run, write down:

* what you changed,
* which stage should change,
* which observation would falsify your hypothesis,
* how long you will wait before declaring the run void.

After the run, record:

* last beacon stage (number and meaning),
* whether the magic was valid (a void page proves nothing),
* whether the screen sequence matched the prediction.

---

## 5. Reading beacons without a full redeploy

If you only need the current beacon page (for example after a manual reset):

```bash
adb shell "su -c 'dd if=/dev/mem bs=256 skip=$((0x4811fc00/256)) count=1 2>/dev/null'" > beacon.bin
adb shell "su -c 'dd if=/dev/mem bs=2048 skip=$((0x4811f800/2048)) count=1 2>/dev/null'" > trace.bin
scripts/decode-beacons.py --beacon beacon.bin --trace trace.bin
```

This reads memory only. It does not load a module and does not reset anything.

---

## 6. Recovery

### Default (hot jump) path

Nothing on storage changed. Power-cycle the phone; Android boots normally.

### If the screen stays black and the phone will not wake

Hold power for ~15 s to force a reset. The hot jump only alters DRAM, so a cold
reset returns to Android.

### If you flashed `boot_b` and it fails

```bash
fastboot flash boot_b stock-boot_b.img
fastboot reboot
```

You need the stock image for your exact model and build. Do not use another
model's `boot_b`; the DTB and the load addresses differ.

### If the phone will not enter fastboot

MediaTek devices expose a BootROM/preloader mode over USB. Recovery from there
needs vendor tooling that is out of scope here; this is the reason `--flash` is
gated behind an explicit flag and a warning.

---

## 7. What has actually been verified

Reproduced from [`BASELINE.md`](BASELINE.md) so this file can be read alone:

| Observation | Status |
| --- | --- |
| Hot jump reaches UEFI | verified |
| SEC/DXE/BDS progress beacons | verified |
| Colour-bar test pattern on the panel | verified |
| UniXNU menu renders with countdown | verified (video) |
| Menu responds to keys | **not verified** — menu reports `No input device` |
| Boot entry present in the menu | **not verified** — `no boot entry found` |
| XNU kernel entry | **not verified** |
| `Darwin Kernel Version` output | **never observed** |

If a run appears to contradict this table, check the measurement before
updating the table. That mistake was made repeatedly on this project.
