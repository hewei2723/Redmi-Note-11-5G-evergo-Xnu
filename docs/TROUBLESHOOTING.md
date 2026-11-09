# Troubleshooting

Symptoms seen on this project, what caused them, and — more usefully — what each
one does and does not prove.

The recurring theme: **several apparent firmware faults were measurement
faults.** Before concluding that code did not run, verify that the thing you are
reading is the thing the code wrote.

---

## Build and dependency problems

### `fetch-deps.sh` fails: "does not apply to ... the upstream revision moved"

The patch in `patches/` targets a specific upstream commit. If a submodule did
not land on the pinned revision, the patch context will not match.

```bash
git -C .work/uefi/Mu_Basecore rev-parse HEAD
# expected: 3c9c7b2e211e62d2a954ad760a36fe05adc43aa3
```

Fix the pin; do not hand-edit third-party sources to make the patch fit. A
silently modified upstream is no longer the thing that was verified.

Note: upstream EDK2/Mu files use CRLF line endings, so the patch is applied with
`git apply --ignore-whitespace`. That is expected, not a workaround for a
corrupt patch.

### `build` reports it cannot find `evergoPkg/evergo.dsc`

`--tree` is pointing at the wrong directory, or `fetch-deps.sh` was never run.
The tree must contain `Platforms/Xiaomi/evergoPkg/evergo.dsc`. The script checks
this and refuses to run otherwise.

### Build fails inside `Mu_Basecore/BaseTools`

The EDK2 base tools are compiled on first use and need `make` and a host C
compiler. On macOS also confirm the Command Line Tools are installed.

### `FD size` mismatch (not 2097152 bytes)

The build ran without the hot-jump geometry. Use `scripts/build.sh`; it always
passes:

```text
-D FD_BASE=0xB0000000 -D FD_SIZE=0x200000 -D FD_BLOCKS=0x200
```

A cold-boot FD (base `0x40080000`) faults the moment the MMU comes on, which
looks like "the firmware is broken" rather than "the image was linked wrong".

### Two builds of identical sources produce different hashes

Expected. The compiler embeds build paths and PDB signatures. What is stable is
the module inventory and the code; compare module bytes, not the FD hash. See
`docs/BASELINE.md` for the comparison method.

---

## Boot problems

### Nothing at all: the phone resets straight back to Android

The jump did not take effect, or the image was rejected before executing.

Check, in order:

1. the stage1 module reported success (its own output),
2. the image is the one you built (`sha256sum` on both sides),
3. the FD is exactly 2 MiB,
4. `BootShim.bin` was rebuilt for the current FD (the shim records the FD size
   and base at assembly time — a stale shim copies the wrong amount).

**Does not prove** that the firmware is broken.

### Colour bars appear, then black

SEC ran and reached the framebuffer, then something after it died.

This is the most informative failure. Read the beacon page: the last stage tells
you whether DXE dispatch completed, whether BDS reached `AfterConsole`, and
whether the UniXNU launch point was reached. See the stage table in the README.

Common causes:

* a driver dispatched from the FV faults (a new INF added to `DXE.inc`),
* the memory map no longer covers an address the next stage touches (adding or
  moving a region in `MemoryMapLib`),
* the FD was rebuilt for a different base than the shim copies to.

### Colour bars, then the UniXNU menu appears and the countdown runs

That is the verified state. It is not a failure and it is not XNU booting.

If the menu does not appear where it used to, the most likely cause is the boot
manager path: `BootManagerLib/BootManager.c` looks for a specific FV file GUID
and starts it. Confirm the file is still in `DXE.inc` and still has that GUID.

### Menu appears, no key works, screen says `No input device`

This is the known state of the verified build, not a regression.

The menu counts input devices that have a device path. On this device no
`SimpleTextIn` handle reaches `gST->ConIn` at the time the menu looks. The
firmware does contain `KeypadDxe` and `KeypadDeviceDxe`; the binding order
relative to the menu is the open question.

Do **not** "fix" this by polling keypad registers directly inside the menu, or
by overriding the vendored UniXNU menu source. Both were tried after the
verified build and neither produced a key event; the second also broke the rule
that the menu is upstream code.

### Menu appears, then the device resets

The countdown expired. The default entry is `Return to firmware`, which makes
the menu exit, which returns to the XNU payload hook, which finds no payload and
returns to BDS, which cannot boot anything and idles until the watchdog resets
the SoC.

Expected behaviour for a firmware with no boot entries and no working input.

### Menu says `no boot entry found`

There is no payload volume published to the menu. That is a missing feature, not
a fault.

---

## Beacon and diagnostic problems

### The beacon page reads all `0x00` or all `0xFF`

The page was overwritten. It lives inside the reserved ramoops window that the
Android kernel manages, and the kernel rewrites parts of it.

**This proves nothing about whether the firmware ran.** Treat the run as void
and either repeat it or read the mirror copy the loader writes into the payload
window.

### The beacon page has a plausible magic but nonsense slots

Same cause. A page that was partially overwritten can still show the magic from
an earlier run. Check the stage against what the screen showed; if they
disagree, believe the screen and re-run.

### A stage marker is missing but later stages are present

Markers are written into a small ring in some paths. A missing intermediate
marker usually means it was overwritten, not skipped. Confirm with a monotonic
counter or a unique per-run id before concluding the code was skipped.

### The trace ring shows records from a previous run

The TRC ring is not cleared on entry in every path. Give each run a unique
marker value and check it before reading the rest.

---

## Deployment problems

### `deploy.sh` refuses: "ro.product.device is '...', expected 'evergo'"

Working as intended. The memory map is device-specific; flashing it to another
model is how devices get bricked.

### `deploy.sh` refuses without `--yes`

Also intended. Every deployment resets the phone.

### `adb` disappears mid-procedure

Expected: the hot jump hands control away from Android. The screen is the only
output until Android returns.

### After the run, the phone does not return to Android

Hold power ~15 s. The hot-jump path only changes DRAM, so a cold reset restores
Android. If it still does not start and you never flashed a partition, the
problem is not this firmware.

---

## Rules that saved time on this project

1. **Fix the measurement before believing the measurement.** Write address, read
   address, cache-maintenance address and the reader's offset must agree. They
   did not, more than once, and each mismatch produced a confident wrong
   conclusion.
2. **One change per run.** Sweeps of speculative edits make every result
   unattributable.
3. **Distinguish "did not execute" from "executed and read zero."** Only a marker
   written by the code under test separates them.
4. **A clean build is not a boot.** Compiler success says nothing about whether
   BDS is reached.
5. **A rendered image is not kernel progress.** UEFI drawing a picture and XNU
   printing to a console are different layers; name the one you observed.
6. **Do not trust a stale artifact.** Build scripts can fail and still leave an
   older FD on disk with a valid-looking hash. Check the build's exit status and
   the artifact's timestamp.
