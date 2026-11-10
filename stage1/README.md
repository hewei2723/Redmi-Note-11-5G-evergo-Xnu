# `evergo_stage1` — the hot-jump kernel module

[**中文**](README.zh-CN.md) | English

This is the device-side half of the boot chain. Everything in the top-level
repository runs on your build machine; this module runs on the phone, inside
Android, as root, and hands control to the UEFI firmware.

It is included because without it the firmware cannot be started without
flashing a partition, and "how do I actually run this" would be unanswerable.

## What it does

Android is running normally. The module:

1. opens the firmware FD from the filesystem,
2. copies it to a physical address the device's own device tree reserves
   (`0xB0000000`), verifying the copy byte-for-byte,
3. optionally copies an `XNUP` payload to the payload window (`0xB5400000`),
4. writes the device-tree pointer where the firmware expects it,
5. stops the other CPUs, turns the MMU off, and branches into the FD.

The SoC never resets: the preloader and LK are skipped entirely, and DRAM keeps
its contents. That is why the beacon pages survive and why a failed attempt is
recovered by power-cycling rather than by reflashing.

## Why it is in `stage1/` and not vendored as a binary

An earlier draft of this repository described the module as "device-side
tooling, not included". That was wrong — the 56 KB of source is the only part
of the boot chain a user could not otherwise reconstruct. The compiled `.ko` is
314 KB and is *not* committed, because it must match the kernel you are running:
same kernel version, same LTO/CFI configuration, same symbol addresses.

## Building it

The module must be built against the prepared Android 5.10 kernel tree for this
device. It is **not** enough to have the kernel sources; the tree must be
configured and built far enough that `Module.symvers` exists.

```bash
# 1. Kernel source. Xiaomi publishes it; the tree used here was:
#    https://github.com/Xiaomi-MT6833/kernel_xiaomi_evergo
export KDIR=~/kernel_xiaomi_evergo

# 2. Prepare it. The vendor defconfig name depends on your source drop; the
#    tree used here was built with the evergo defconfig and LTO+CFI on.
make -C "$KDIR" ARCH=arm64 O="$KDIR/out" evergo_defconfig
make -C "$KDIR" ARCH=arm64 O="$KDIR/out" CC=clang LD=ld.lld \
     CONFIG_LTO_CLANG=y CONFIG_CFI_CLANG=y modules_prepare

# 3. Build the module.
cd stage1
make KDIR="$KDIR/out" \
     CLANG_DIR=~/android-ndk-r30/toolchains/llvm/prebuilt/linux-x86_64/bin
```

Output: `evergo_stage1.ko` in `stage1/`.

The defconfig name and the exact kernel revision are the two things most likely
to differ on your source drop. Check `arch/arm64/configs/` for the `evergo*`
file, and compare `uname -r` on the phone with the tree you built from.

### LTO and CFI are not optional

The vendor kernel CFI-checks the `mod->init` indirect call. A module built
without `CONFIG_LTO_CLANG=y CONFIG_CFI_CLANG=y` either fails to load or faults.
This is the single most common reason a self-built module does not work, and it
does not produce an obvious error message.

## Loading it

The module takes many parameters. The ones mode 5 (the hot jump) needs are
symbol addresses read from the running kernel, which is why the invocation is a
shell script rather than one line you can memorise:

```bash
su -c '
  S=$(awk "\$3==\"smp_send_stop\"{print \$1}" /proc/kallsyms)
  F=$(awk "\$3==\"filp_open\"{print \$1}" /proc/kallsyms)
  R=$(awk "\$3==\"kernel_read\"{print \$1}" /proc/kallsyms)
  FA=$(awk "\$3==\"__flush_dcache_area\"{print \$1}" /proc/kallsyms)
  taskset 1 insmod /data/adb/evergo_stage1.ko mode=5 stop=1 \
    fd_path=/data/adb/evergo-hotjump.fd \
    p_fd_base=0xb0000000 p_jump_pa=0xb0000000 \
    p_smp_send_stop=0x$S p_filp_open=0x$F p_kernel_read=0x$R \
    p_flush_dcache_area=0x$FA p_flush_pa=0x0 p_flush_len=0x0
'
```

`fd_path` is the **2 MiB FD** from `scripts/build.sh`, not the boot image from
`scripts/make-boot-image.sh`. The module copies the FD directly; the boot-image
wrapper exists for the `--flash` path, where LK does the unpacking.

`taskset 1` pins the module to CPU 0, matching the assumptions in the
trampoline. `stop=1` halts the other cores before the jump.

## Failure modes that look like something else

| Symptom | Cause |
| --- | --- |
| `insmod` fails with `Invalid argument` / no message | built without LTO+CFI |
| `stage1: cannot open /data/adb/...` | wrong path, or SELinux blocking; run through `su -mm` |
| `stage1: FD has no _FVH at 0x28` | you passed a boot image instead of the raw FD |
| jumps, then black screen with no colour bars | FD copied but the entry point faulted; suspect wrong `p_jump_pa` or an FD linked for a different base |
| jumps and immediately returns to Android | `p_jump_pa`/`p_fd_base` do not match where the FD was copied |

## Safety

This module writes to reserved DRAM only. It does not modify any partition, and
a cold reset restores Android. That is a property of this specific code path,
not a general guarantee: the module also contains diagnostic modes (1, 3, 9)
that deliberately reset the SoC or stop CPUs, and mode 3 never returns.

Do not enable the commented-out `uefi_jump.o` in the Makefile. Its init writes
`0x98000000`/`0x9C000000`, which are modem-reserved, and that caused an EMI
violation storm and a panic on 2026-11-04.

## Design notes

The source header refers to `PHASE1-SAFE-TEST-DESIGN.md`, an internal design
document that is not published here: it is a working engineering log (in
Chinese) of the diagnostic modes, with per-observation evidence tags, and it
describes test procedures that were never run on hardware. It is not needed to
build or use the module.

What it establishes, and what matters here, is the safety rule the code
follows for the hot-jump path: no fixed DRAM physical address is written except
the two the device tree already reserves (`0xB0000000` for the FD,
`0xB5400000` for the payload), and after `smp_send_stop()` only inline assembly
runs.

## Licence

`GPL-2.0`, as required by the kernel module interface. See `../LICENSE` for the
rest of the repository; the two do not conflict because the module is a
separate work that links against the kernel rather than the firmware.
