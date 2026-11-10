# Evergo UEFI Jump — Magisk module

A home-screen icon that starts the evergo UEFI firmware. Tap it, confirm, and
the phone hands control to the firmware without touching any partition.

This is a convenience wrapper around the same operation as
`scripts/deploy.sh --fd` on a build machine. It exists so you do not need a
computer and `adb` every time you want to look at the firmware.

## What it installs

| Path (inside the module) | What |
| --- | --- |
| `system/bin/evergo-jump` | the script that performs the jump |
| `evergo_stage1.ko` | the hot-jump kernel module (built from `stage1/`) |
| `fd/evergo-hotjump.fd` | the verified firmware, 2 MiB |
| `EvergoJump.apk` | the launcher icon |

The APK is installed as a normal user app. Everything else lives in
`/data/adb/modules/evergo-jump/`.

## Requirements

* **Redmi Note 11 5G (evergo, MT6833).** The installer refuses any other
  `ro.product.device`. The FD and the memory map are specific to this model.
* **Magisk**, with root working from a shell.
* **Kernel `4.14.186-perf-g75f5627fcaea`.** The bundled `.ko` only loads on that
  kernel. Check with `adb shell uname -r`; on anything else, rebuild the module
  from `stage1/` and drop it into the module directory.

## Install

1. Copy `evergo-jump-v1.0.zip` to the phone.
2. Magisk app → Modules → Install from storage → pick the zip.
3. Reboot (not strictly required, but the APK install is cleaner after one).
4. An **Evergo Jump** icon appears in the app drawer.

Or from a shell:

```bash
magisk --install-module /sdcard/evergo-jump-v1.0.zip
```

## Use

1. Tap **Evergo Jump**.
2. Read the confirmation. It says what will happen and how to recover.
3. Tap **Start**.

The screen goes to the firmware: colour bars, then the UniXNU menu with a
running countdown. Android is gone until you power-cycle.

If the jump does not start, the app shows the script's output instead of
leaving you guessing. The same log is at `/data/local/tmp/evergo-jump.log`.

From a terminal, without the icon:

```bash
su -c evergo-jump
```

## Recovery

The jump only changes DRAM. Nothing on storage is written, so:

* **Black screen, phone will not wake:** hold power for ~15 seconds.
* **Android boots normally after that:** the jump simply did not take effect.

There is no scenario in which this module leaves the phone unable to boot
Android, because no partition is touched. The `--flash` path in
`scripts/deploy.sh` is the one that can brick a device, and this module does not
use it.

## What you will see, and what it means

| On screen | Meaning |
| --- | --- |
| colour bars | SEC has taken over the framebuffer |
| UniXNU menu with countdown | the firmware and boot manager work |
| menu says `No input device` | expected; menu input is not implemented |
| menu says `no boot entry found` | expected; no payload is published |
| nothing, then Android returns | the jump did not take effect; check the log |

**The menu appearing is not XNU booting.** XNU does not start on this device
yet. See the main README's verification table.

## Removing it

Magisk app → Modules → remove **Evergo UEFI Jump** → reboot. The APK is a
normal app and can be uninstalled separately.

## Licence

The module wrapper and the APK are BSD-2-Clause-Patent, like the rest of the
repository. `evergo_stage1.ko` is GPL-2.0, as the kernel module interface
requires. The firmware FD is built from this repository's sources.
