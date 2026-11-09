/**
  Copyright (c) 2011-2017, ARM Limited. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/PcdLib.h>
#include <Library/DebugLib.h>
#include <Library/PrePiLib.h>
#include <Library/DebugAgentLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/SerialPortLib.h>
#include <Library/PlatformSecLib.h>
#include <Library/PrePiHobListPointerLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/TimerLib.h>
#include <Library/HobLib.h>
#include <Library/EvergoTrace.h>
#include <Library/FrameBufferMap.h>

#include "Sec.h"

//
// Test-D beacon: record SEC progress into the reserved FD tail (FD base +
// 0x300000). The region is protected by the boot DTB reserved-memory node
// (uefi-fd@B0000000), so the values survive a watchdog reset and can be read
// back by the evergo_stage1 module to locate silent-failure points.
//
#define SEC_BEACON_BASE  0x4811FC00ULL  /* reserved ramoops page; read back by the evergo_stage1 module */
/* Mirror inside the reserved XNUP payload window: the ramoops page is rewritten
 * by the Android kernel at every boot, so the copy that proves the run is this
 * one. Nothing else maps or writes 0xB6BE0000. */
#define SEC_BEACON_MIRROR 0xB6BE0000ULL
#define SEC_BEACON_MAGIC 0x53473142ULL  /* "B1GS" little-endian */
#define SEC_BEACON_SLOTS 32             /* 256 B: C-beacon area ends before the asm beacon at +0x100 */

// Slot map (u64 each; 256 B total, C-beacon area ends before asm beacon):
//   0       magic
//   1       last normal step (PRESERVED when an exception occurs)
//   2       'tram' - owned by the kernel-side stage1 module, FD never writes
//   3..17   bounded HOB summary v5 (SecDumpHobChain):
//             3: (WalkedCount<<32)|Flags
//             4: (StopType<<48)|(StopLen<<32)
//             5: stop-point HOB PA
//             6: chain base (PHIT)
//             7: 0xDEADBEEF00000005 (summary version marker)
//             8: culprit PA (HOB whose Len jumps past PHIT.EfiEndOfHobList)
//             9: culprit (Type<<32)|(Len<<16)|Index
//            10: header AT EfiEndOfHobList ((Type<<32)|(Len<<16))
//            11: PHIT.EfiEndOfHobList (+0x30)
//            12: PHIT.EfiFreeMemoryBottom (+0x28)
//            13: PHIT.EfiFreeMemoryTop (+0x20)
//            14..15: zero; no additional ramoops storage is used
//            16: (Flags<<32)|3 after summary completion
//            17: spare
//   18..26  exception snapshot: ELR, ESR, FAR, SP, ExcType, SCTLR, TTBR0, TCR, EL|DAIF
//   27      0xEE00000000000000 | (LastNormalStep<<32) | ExcType
//   28..31  interrupted x0..x3
#define SEC_BEACON_SLOT_HOB_BASE   3
#define SEC_BEACON_HOB_MAX_STEPS   4096   /* dump-walk iteration cap */
#define SEC_BEACON_SLOT_EXC_BASE   18
#define SEC_BEACON_SLOT_EXC_MARK   27
#define SEC_BEACON_SLOT_X0         28
//
// Test-D recovery watchdog. PlatformInitialize() disables the MTK watchdog, so
// any fault inside SEC/DXE hangs the device with no log and no reset. Re-arm a
// finite 15 s reset and kick it from every beacon: a hang now self-recovers and
// the beacons show the last step reached. Registers follow
// drivers/watchdog/mtk_wdt.c (LENGTH = (secs << 11) | 8, MODE key 0x22000000).
//
#define EVERGO_TOPRGU_BASE  0x10007000ULL
// The TOPRGU WDT_LENGTH field is only 5 bits wide (bits 15:11), so the usable
// range is 1..31 seconds. 60 was silently truncated into an invalid value and
// produced unpredictable resets. Keep it at the largest value that fits.
#define EVERGO_WDT_SECONDS  31
#define EVERGO_WDT_ENABLE   1   /* 1 = armed safety net, 0 = diagnostic */

STATIC
VOID
SecWatchdogKick (
  VOID
  )
{
  volatile UINT32 *Wdt = (volatile UINT32 *)EVERGO_TOPRGU_BASE;

  Wdt[2] = 0x1971;                /* WDT_RST reload */
  asm volatile ("dsb sy" ::: "memory");
}

STATIC
VOID
SecWatchdogArm (
  VOID
  )
{
  volatile UINT32 *Wdt = (volatile UINT32 *)EVERGO_TOPRGU_BASE;

  Wdt[2] = 0x1971;                                  /* reload first */
  Wdt[1] = (EVERGO_WDT_SECONDS << 11) | 0x8;        /* LENGTH + key */
#if EVERGO_WDT_ENABLE
  Wdt[0] = 0x22000000 | 0x1 | 0x4;                  /* KEY | EN | EXRST_EN */
#else
  /* Diagnostic: leave it disarmed so a long loader/kernel phase is not cut
   * short. With this off a hung kernel needs a manual power-hold reset. */
  Wdt[0] = 0x22000000 | 0x4;                        /* KEY | EXRST_EN, EN=0 */
#endif
  asm volatile ("dsb sy" ::: "memory");
}

/*
 * Container-head snapshot: record the XNUP magic at 0xB5400000 into the SEC
 * beacon page slots 0x20/0x24 (BEFORE MMU on) at several stages, so the
 * Android-side reader can see exactly when the container header disappears.
 * Slot base 0x4811FC00 (SEC_BEACON_BASE) + 0x20 = magic, +0x24 = size field.
 * Call order is identified by the SecBeacon step value in beacon slot 1.
 */
STATIC
VOID
SecContainerSnapshot (
  IN UINTN  T
  )
{
  /* unique slot per stage: T1->24..T3->26, T6->29, T7->30 (slots 5-17 taken
   * by the SEC HOB-summary beacon; 20-31 free).
   * Encoding: bits56-63 = stage tag T ("executed" marker, zero = not run),
   * bits32-55 = container size field, bits0-31 = container magic. */
  volatile UINT32 *Cont = (volatile UINT32 *)0xB5400000ULL;
  volatile UINT64 *Slot = (volatile UINT64 *)(SEC_BEACON_BASE + (23 + T) * 8);

  Slot[0] = ((UINT64)(T & 0xFF) << 56) | ((UINT64)Cont[1] << 32) | Cont[0];
  asm volatile ("dsb sy" ::: "memory");
  {
    asm volatile ("dc cvac, %0" :: "r" (SEC_BEACON_BASE + (23 + T) * 8) : "memory");
    asm volatile ("dsb sy" ::: "memory");
  }
}

STATIC
VOID
SecBeacon (
  IN UINTN  Step
  )
{
  volatile UINT64 *Beacon = (volatile UINT64 *)SEC_BEACON_BASE;

  Beacon[0] = SEC_BEACON_MAGIC;
  Beacon[1] = (UINT64)Step;
  {
    volatile UINT64 *Mirror = (volatile UINT64 *)SEC_BEACON_MIRROR;
    Mirror[0] = SEC_BEACON_MAGIC;
    Mirror[1] = (UINT64)Step;
  }
  asm volatile ("dsb sy" ::: "memory");
  SecWatchdogKick ();
  // Pre-MMU this is a harmless no-op; after MemoryPeim enables the MMU and
  // D-cache it is ESSENTIAL - without the clean the beacon values live only
  // in dirty cache lines and are lost at reset.
  WriteBackDataCacheRange ((VOID *)SEC_BEACON_BASE, SEC_BEACON_SLOTS * sizeof (UINT64));
  WriteBackDataCacheRange ((VOID *)SEC_BEACON_MIRROR, SEC_BEACON_SLOTS * sizeof (UINT64));
}

// Called from MemoryPeim (MemoryInitPeiLib) to bisect InitializeMemory().
VOID
SecBeaconHook (
  IN UINTN  Step
  )
{
  SecBeacon (Step);
}

//
// Structured exception snapshot, written by the SEC exception handler
// (ArchSec.c) before it dead-loops. Registers come from the interrupted
// context: ELR/SP/x0..x3 via Exception.S, the rest read banked on CurrentEL.
// The last normal progress step (slot 1) is PRESERVED: slot 27 carries the
// 0xEE marker plus that step, so a fault never erases progress evidence.
//
VOID
SecExceptionCapture (
  IN UINTN  ExcType,
  IN UINTN  Elr,
  IN UINTN  Sp,
  IN UINTN  Esr,
  IN UINTN  Far,
  IN UINTN  Sctlr,
  IN UINTN  Ttbr0,
  IN UINTN  Tcr,
  IN UINTN  ElDaif,
  IN UINTN  OrigX0,
  IN UINTN  OrigX1,
  IN UINTN  OrigX2,
  IN UINTN  OrigX3
  )
{
  volatile UINT64 *Beacon = (volatile UINT64 *)SEC_BEACON_BASE;
  UINTN            LastStep;

  LastStep = (UINTN)Beacon[1];

  Beacon[SEC_BEACON_SLOT_EXC_BASE + 0] = Elr;     // faulting PC
  Beacon[SEC_BEACON_SLOT_EXC_BASE + 1] = Esr;     // syndrome (EC/IL/FAR-valid...)
  Beacon[SEC_BEACON_SLOT_EXC_BASE + 2] = Far;     // faulting address
  Beacon[SEC_BEACON_SLOT_EXC_BASE + 3] = Sp;      // interrupted SP
  Beacon[SEC_BEACON_SLOT_EXC_BASE + 4] = ExcType;
  Beacon[SEC_BEACON_SLOT_EXC_BASE + 5] = Sctlr;
  Beacon[SEC_BEACON_SLOT_EXC_BASE + 6] = Ttbr0;
  Beacon[SEC_BEACON_SLOT_EXC_BASE + 7] = Tcr;
  Beacon[SEC_BEACON_SLOT_EXC_BASE + 8] = ElDaif;  // (CurrentEL<<8)|DAIF

  Beacon[SEC_BEACON_SLOT_EXC_MARK] =
    0xEE00000000000000ULL | ((UINT64)LastStep << 32) | (UINT64)(ExcType & 0xFFFFFFFF);

  Beacon[SEC_BEACON_SLOT_X0 + 0] = OrigX0;        // e.g. CreateHob's PHIT pointer
  Beacon[SEC_BEACON_SLOT_X0 + 1] = OrigX1;
  Beacon[SEC_BEACON_SLOT_X0 + 2] = OrigX2;
  Beacon[SEC_BEACON_SLOT_X0 + 3] = OrigX3;

  // Refresh the magic/step markers and clean everything in one sweep; the
  // watchdog stays armed so the device self-recovers after the snapshot.
  Beacon[0] = SEC_BEACON_MAGIC;
  asm volatile ("dsb sy" ::: "memory");
  SecWatchdogKick ();
  WriteBackDataCacheRange ((VOID *)SEC_BEACON_BASE, SEC_BEACON_SLOTS * sizeof (UINT64));
}

//
// Bounded HOB summary, version 5. Writes only the existing 256-byte beacon.
// Flags: 1=valid END, 2=bad length, 4=zero header, 8=iteration cap,
// 16=length crosses declared END, 64=bad PHIT/range/alignment,
// 128=invalid/early END. Slots 14/15 remain zero (no pstore image copy).
// These checks establish structural bounds, not RAM ownership or coherency.
//
STATIC
VOID
SecDumpHobChain (
  VOID
  )
{
  volatile UINT64                 *Beacon;
  volatile EFI_HOB_GENERIC_HEADER *Hob;
  volatile EFI_HOB_HANDOFF_INFO_TABLE *Phit;
  EFI_MEMORY_REGION_DESCRIPTOR   Region;
  UINTN                          Base;
  UINTN                          Addr;
  UINTN                          Limit;
  UINTN                          Count;
  UINTN                          Flags;
  UINTN                          HobType;
  UINTN                          HobLen;
  UINT64                         EndHob;
  UINT64                         FreeBottom;
  UINT64                         FreeTop;
  UINT64                         AtEndPacked;

  Beacon      = (volatile UINT64 *)SEC_BEACON_BASE;
  Base        = (UINTN)PrePeiGetHobList ();
  Addr        = Base;
  Count       = 0;
  Flags       = 64;
  HobType     = 0;
  HobLen      = 0;
  EndHob      = 0;
  FreeBottom  = 0;
  FreeTop     = 0;
  AtEndPacked = 0;
  ZeroMem (&Region, sizeof (Region));
  LocateMemoryRegionByName ("DXE Heap", &Region);

  // Validate the pointer against the static mapped heap before dereferencing.
  if ((Region.Length < sizeof (*Phit)) ||
      (Region.Address > MAX_UINTN - Region.Length)) {
    goto Record;
  }

  Limit = (UINTN)(Region.Address + Region.Length);
  if (((Base & 7) != 0) || (Base < Region.Address) ||
      (Base > Limit - sizeof (*Phit))) {
    goto Record;
  }

  Phit       = (volatile EFI_HOB_HANDOFF_INFO_TABLE *)Base;
  EndHob     = Phit->EfiEndOfHobList;
  FreeBottom = Phit->EfiFreeMemoryBottom;
  FreeTop    = Phit->EfiFreeMemoryTop;
  if ((Phit->Header.HobType != EFI_HOB_TYPE_HANDOFF) ||
      (Phit->Header.HobLength != sizeof (*Phit)) ||
      ((EndHob & 7) != 0) || (EndHob < Base + sizeof (*Phit)) ||
      (EndHob > Limit - sizeof (*Hob)) ||
      (FreeBottom != EndHob + sizeof (*Hob)) ||
      (FreeTop < FreeBottom) || (FreeTop > Limit)) {
    goto Record;
  }

  Flags = 0;
  for (Count = 0; Count < SEC_BEACON_HOB_MAX_STEPS; Count++) {
    // Never read outside the declared list, including at the iteration cap.
    if (((Addr & 7) != 0) || (Addr > EndHob)) {
      Flags |= 64;
      break;
    }

    Hob     = (volatile EFI_HOB_GENERIC_HEADER *)Addr;
    HobType = Hob->HobType;
    HobLen  = Hob->HobLength;
    if (Addr == EndHob) {
      AtEndPacked = ((UINT64)HobType << 32) | ((UINT64)HobLen << 16);
      Flags |= ((HobType == EFI_HOB_TYPE_END_OF_HOB_LIST) &&
                (HobLen == sizeof (*Hob))) ? 1 : 128;
      break;
    }

    if (HobType == EFI_HOB_TYPE_END_OF_HOB_LIST) {
      Flags |= 128;
      break;
    }

    if ((HobLen < sizeof (*Hob)) || ((HobLen & 7) != 0)) {
      Flags |= 2;
      if ((HobType == 0) && (HobLen == 0)) {
        Flags |= 4;
      }
      break;
    }

    if (HobLen > EndHob - Addr) {
      Flags |= 16;
      break;
    }

    Addr += HobLen;
  }

  if (Count == SEC_BEACON_HOB_MAX_STEPS) {
    Flags |= 8;
  }

Record:
  Beacon[3]  = ((UINT64)Count << 32) | Flags;
  Beacon[4]  = ((UINT64)HobType << 48) | ((UINT64)HobLen << 32);
  Beacon[5]  = Addr;
  Beacon[6]  = Base;
  Beacon[7]  = 0xDEADBEEF00000005ULL;
  Beacon[8]  = ((Flags & 1) == 0) ? Addr : 0;
  Beacon[9]  = ((Flags & 1) == 0) ?
               (((UINT64)HobType << 32) | ((UINT64)HobLen << 16) | Count) : 0;
  Beacon[10] = AtEndPacked;
  Beacon[11] = EndHob;
  Beacon[12] = FreeBottom;
  Beacon[13] = FreeTop;
  Beacon[14] = 0;
  Beacon[15] = 0;
  Beacon[16] = ((UINT64)Flags << 32) | 3;
  Beacon[17] = 0;
  asm volatile ("dsb sy" ::: "memory");
  WriteBackDataCacheRange ((VOID *)SEC_BEACON_BASE, SEC_BEACON_SLOTS * sizeof (UINT64));
}

#ifndef MDEPKG_NDEBUG
STATIC
VOID
ClearFrameBuffer (VOID)
{
  EFI_MEMORY_REGION_DESCRIPTOR FrameBufferRegion;

  // Locate "Display Reserved" Memory Region
  LocateMemoryRegionByName ("Display Reserved", &FrameBufferRegion);
  LocateMemoryRegionByName ("Display_Reserved", &FrameBufferRegion);

  // Clear Memory Region
  if (FrameBufferRegion.Address != 0 && FrameBufferRegion.Length != 0) {
    ZeroMem ((VOID *)FrameBufferRegion.Address, FrameBufferRegion.Length);
  }
}

STATIC
__attribute__((unused))
VOID
PrintFirmwareVersion (VOID)
{
  // Clear Frame Buffer
  ClearFrameBuffer ();

  // Print UEFI Version Message
  DEBUG ((EFI_D_WARN, "\n"));
  DEBUG ((EFI_D_WARN, "Project Silicium for %a %a\n",            FixedPcdGetPtr (PcdSmbiosSystemManufacturer), FixedPcdGetPtr (PcdSmbiosSystemModel)));
  DEBUG ((EFI_D_WARN, "Firmware Version %s Build at %a on %a\n", PcdGetPtr (PcdFirmwareVersionString), __TIME__, __DATE__));
  DEBUG ((EFI_D_WARN, "\n"));
}
#endif

STATIC
EFI_STATUS
InitializeMemory (VOID)
{
  EFI_STATUS                    Status;
  EFI_MEMORY_REGION_DESCRIPTOR  UefiMemoryRegion;
  EFI_HOB_HANDOFF_INFO_TABLE   *HobList;

  // Locate "DXE Heap" Memory Region
  LocateMemoryRegionByName ("DXE Heap", &UefiMemoryRegion);
  LocateMemoryRegionByName ("DXE_Heap", &UefiMemoryRegion);
  SecBeacon (0x41);  // DXE Heap region located
  SecContainerSnapshot (1);  /* T1: earliest SEC view */

  // Save UEFI Memory Details
  UINTN UefiMemoryBase = UefiMemoryRegion.Address;
  UINTN UefiMemorySize = UefiMemoryRegion.Length;

  // Verify UEFI Memory Base
  if (UefiMemoryBase == 0 && UefiMemorySize == 0) {
    DEBUG ((EFI_D_ERROR, "Failed to Locate \"DXE Heap\" Memory Region!\n"));
    return EFI_NOT_FOUND;
  }
  SecBeacon (0x42);  // DXE Heap valid

  // Initialize HOB List
  HobList = HobConstructor ((VOID *)UefiMemoryBase, UefiMemorySize, (VOID *)UefiMemoryBase, (VOID *)(UefiMemoryBase + UefiMemorySize));
  PrePeiSetHobList (HobList);
  SecBeacon (0x43);  // HOB list ready
  SecContainerSnapshot (2);  /* T2: after HobConstructor */

  // Initialize MMU & Memory HOBs
  SecBeacon (0x44);  // entering MemoryPeim
  Status = MemoryPeim (UefiMemoryBase, UefiMemorySize);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  SecBeacon (0x45);  // MemoryPeim returned

  // Build Stack HOB
  BuildStackHob (FixedPcdGet64 (PcdCPUCoresStackBase), FixedPcdGet32 (PcdCPUCorePrimaryStackSize));
  SecBeacon (0x46);  // stack HOB built
  SecContainerSnapshot (3);  /* T3: after MemoryPeim+MMU on */

  return EFI_SUCCESS;
}

//
// Hot-jump DTB publication.
//
// On a cold boot BootShim.S copies FD_BASE and writes LK's DTB pointer (x0)
// into the handoff page at 0xA40007F8. The hot-jump path skips BootShim, so
// that cell holds stale RAM and CamelliaBootMemoryEntry (FVMAIN entry at 0xF4,
// immediately before SimpleFbDxe at 0x104) silently returns without publishing
// the framebuffer handoff or the initrd block device. Publishing a VALIDATED
// FDT pointer here is what lets it publish both, and therefore what lets
// SimpleFbDxe install a GOP.
//
// The candidate is the boot image header's tags_addr (0x47C80000), already
// mapped as "Boot DTB" in MemoryMapLib. Nothing is published unless the memory
// actually looks like an FDT, so a wrong guess degrades to today's behaviour.
//
#define EVERGO_DTB_CANDIDATE     0x47C80000ULL
#define EVERGO_DTB_POINTER_ADDR  0xA40007F8ULL
#define EVERGO_DTB_MAGIC         0xD00DFEEDU
#define EVERGO_DTB_MAX_SIZE      0x00100000U
#define EVERGO_DTB_WINDOW_SIZE    0x00030000ULL   // MemoryMapLib "Boot DTB" length

STATIC
BOOLEAN
SecIsValidFdt (
  IN UINT64  Base
  )
{
  UINT32 Magic;
  UINT32 TotalSize;
  UINT32 OffStruct;

  if ((Base == 0) || ((Base & 7) != 0)) {
    return FALSE;
  }

  Magic = *(volatile UINT32 *)(UINTN)Base;
  if (Magic != EVERGO_DTB_MAGIC) {
    return FALSE;
  }

  TotalSize = *(volatile UINT32 *)(UINTN)(Base + 4);
  if ((TotalSize < 0x100) || (TotalSize > EVERGO_DTB_MAX_SIZE)) {
    return FALSE;
  }

  OffStruct = *(volatile UINT32 *)(UINTN)(Base + 8);
  if ((OffStruct < 0x38) || (OffStruct >= TotalSize)) {
    return FALSE;
  }

  return TRUE;
}

//
// Visible proof that we own the panel, drawn from SEC before any DXE driver
// exists. Per AUDIT-2026-11-06-framebuffer.md §3.3 the primary target
// 0x7C610000 gets a single unambiguous MAGENTA fill (no bars, so the screen
// colour alone identifies the run); the suspect 0x78170000 keeps the bar test
// purely as an "does this address exist" probe. Before overwriting, the
// original words are read back and recorded; after writing, three points are
// read back and folded into a mask recorded via TRC.
//
#define SEC_FB_PRIMARY    0x7C610000ULL   /* [DT] mblock-20 framebuffer */
#define SEC_FB_MAGENTA    0x00FF00FFU     /* little-endian bytes FF 00 FF 00 */
#define SEC_FB_WIDTH      1088U
#define SEC_FB_HEIGHT     2400U
#define SEC_FB_STRIDE_DW  (4352U / 4U)

//
// Same three probe points, but resolved through the page map: a run-scattered
// scanout has no single linear base, so a raw pointer plus offset would read
// the wrong page (or unmapped memory) even though the fill was correct.
//
STATIC
UINT32
SecFbReadbackMaskFromMap (
  IN FB_PAGE_MAP  *Map,
  IN UINT32        Pitch
  )
{
  volatile UINT32  *P;
  UINT32            Mask = 0;

  P = (volatile UINT32 *)FbPageMapTranslate (Map, 0);
  if ((P != NULL) && (*P == SEC_FB_MAGENTA)) {
    Mask |= 1;
  }

  P = (volatile UINT32 *)FbPageMapTranslate (
                            Map,
                            (UINT64)(SEC_FB_HEIGHT / 2) * Pitch +
                            (UINT64)(SEC_FB_WIDTH / 2) * sizeof (UINT32)
                            );
  if ((P != NULL) && (*P == SEC_FB_MAGENTA)) {
    Mask |= 2;
  }

  P = (volatile UINT32 *)FbPageMapTranslate (
                            Map,
                            (UINT64)(SEC_FB_HEIGHT - 1) * Pitch +
                            (UINT64)(SEC_FB_WIDTH - 1) * sizeof (UINT32)
                            );
  if ((P != NULL) && (*P == SEC_FB_MAGENTA)) {
    Mask |= 4;
  }

  return Mask;
}

STATIC
VOID
SecPaintMagenta (
  IN UINT64  Base,
  IN UINT64  Stage
  )
{
  //
  // The live scanout pitch can differ from the legacy 4352-byte constant
  // (observed 34816 on the hot-jump path), so take the stride from the
  // FB_HANDOFF the producer wrote. A wrong stride smears the fill across
  // the wrong rows and the panel shows nothing recognisable.
  //
  volatile UINT32  *H  = (volatile UINT32 *)(UINTN)0xA4000000ULL;   /* handoff words */
  UINT32            Pitch = H[6];
  UINT32            StrideDw;
  UINT64            Orig0;
  UINT64            OrigMid;
  FB_PAGE_MAP      *Map = (FB_PAGE_MAP *)(UINTN)FB_PAGE_MAP_ADDR;
  UINT32            Flags = *(volatile UINT32 *)(
                            (UINT8 *)(UINTN)FB_PAGE_MAP_ADDR + FB_MAP_FLAGS_OFFSET);

  StrideDw = (Pitch >= 4352U && Pitch <= 65536U) ? (Pitch / 4U) : SEC_FB_STRIDE_DW;

  //
  // FBDC/AFBC layer: the buffer carries a header before the body and a linear
  // fill would be decoded as compressed data. Fail closed - record and skip.
  //
  if ((Flags & FB_MAP_FLAG_FBDC) != 0) {
    TRC (Stage, TRC_ERROR, 2, Base, Flags, 0, 0);
    return;
  }

  //
  // The producer describes the scanout as physical runs because ION/dmabuf is
  // contiguous only in IOVA space. Require a valid covering map and fill
  // through it. Fail closed when absent: a raw Base+offset fill could land in
  // the wrong pages or unmapped memory.
  //
  if (FbPageMapIsValid (Map) &&
      (Map->LogicalSize >= (UINT64)SEC_FB_WIDTH * SEC_FB_HEIGHT * sizeof (UINT32))) {
    volatile UINT32  *P0;
    volatile UINT32  *PMid;

    P0   = (volatile UINT32 *)FbPageMapTranslate (Map, 0);
    PMid = (volatile UINT32 *)FbPageMapTranslate (
                                Map,
                                (UINT64)(SEC_FB_HEIGHT / 2) * Pitch +
                                (UINT64)(SEC_FB_WIDTH / 2) * sizeof (UINT32)
                                );
    if ((P0 == NULL) || (PMid == NULL)) {
      TRC (Stage, TRC_ERROR, 4, Base, 0, 0, 0);
      return;
    }
    Orig0   = *P0;
    OrigMid = *PMid;
    TRC (Stage, TRC_ENTER, Map->RunCount, Map->Runs[0].Physical, Orig0, OrigMid,
         (UINT64)SEC_FB_WIDTH | ((UINT64)SEC_FB_HEIGHT << 16) |
         ((UINT64)StrideDw << 32));

    FbPageMapFill32 (Map, 0, SEC_FB_MAGENTA, SEC_FB_WIDTH * SEC_FB_HEIGHT);

    asm volatile ("dsb sy" ::: "memory");
    TRC (Stage, TRC_EXIT, (UINT64)SecFbReadbackMaskFromMap (Map, Pitch),
         (UINT64)SEC_FB_WIDTH * SEC_FB_HEIGHT * 4, StrideDw, 0, 0);
    return;
  }

  //
  // No valid covering map: fail closed. The legacy linear fallback is gone on
  // purpose - without a proven map, painting would be a guess.
  //
  TRC (Stage, TRC_ERROR, 3, Base, 0, 0, 0);
}


STATIC
VOID
SecPublishBootDtb (
  VOID
  )
{
  volatile UINT64  *Beacon;
  volatile UINT64  *Slot;
  UINT64            Address;
  UINT64            Found;

  Beacon = (volatile UINT64 *)SEC_BEACON_BASE;
  Slot   = (volatile UINT64 *)(UINTN)EVERGO_DTB_POINTER_ADDR;
  Found  = 0;

  if (SecIsValidFdt (EVERGO_DTB_CANDIDATE)) {
    Found = EVERGO_DTB_CANDIDATE;
  } else {
    //
    // LK normally copies the DT to tags_addr, but do not bet the boot on an
    // exact offset: walk the declared window by FDT header instead. The window
    // is the mapped "Boot DTB" region, so every read below is inside it.
    //
    for (Address = EVERGO_DTB_CANDIDATE;
         (Address + 0x40) <= (EVERGO_DTB_CANDIDATE + EVERGO_DTB_WINDOW_SIZE);
         Address += 8)
    {
      if (SecIsValidFdt (Address)) {
        Found = Address;
        break;
      }
    }
  }

  if (Found == 0) {
    // Nothing usable: publish nothing and keep the observed first word so the
    // next run can tell "zeroed" from "other data" from "unmapped".
    Beacon[17] = *(volatile UINT64 *)(UINTN)EVERGO_DTB_CANDIDATE;
    SecBeacon (0x49);
    return;
  }

  *Slot = Found;
  asm volatile ("dsb sy" ::: "memory");
  Beacon[17] = Found;
  SecBeacon (Found == EVERGO_DTB_CANDIDATE ? 0x48 : 0x4A);
}

STATIC
VOID
SecMain (VOID)
{
  EFI_STATUS Status;

  SecBeacon (1);  // entered from ResetVector

  // Do Platform Specific Initialization
  PlatformInitialize ();
  // Arm a finite watchdog so a later fault self-resets instead of hanging.
  SecWatchdogArm ();
  SecBeacon (2);  // platform init done

#ifndef MDEPKG_NDEBUG
  // Initialize Serial Port
  Status = SerialPortInitialize ();
  if (EFI_ERROR (Status)) {
    SecBeacon (0xF3);
    return;
  }
  SecBeacon (3);  // serial up

  // BISECT: PrintFirmwareVersion (DEBUG/UART writes) and InitializeDebugAgent
  // are temporarily disabled to locate the hot-jump crash point.
  // PrintFirmwareVersion ();
  SecBeacon (4);  // banner block skipped

  // InitializeDebugAgent (DEBUG_AGENT_INIT_POSTMEM_SEC, NULL, NULL);
  // SaveAndSetDebugTimerInterrupt (TRUE);
#endif

  // Initialize Memory
  TRC (TRC_SEC_MEMORY, TRC_ENTER, 0, 0, 0, 0, 0);
  Status = InitializeMemory ();
  TRC (TRC_SEC_MEMORY, TRC_EXIT, Status, 0, 0, 0, 0);
  if (EFI_ERROR (Status)) {
    SecBeacon (0xF5);
    return;
  }
  SecBeacon (5);  // memory + HOBs ready

  // Publish a validated bootloader DTB pointer for CamelliaBootMemoryEntry.
  // 0x48 published, 0x49 not found (slot 17 keeps the observed value).
  //
  // Paint the buffer the DPU is ACTUALLY scanning. The hot-jump module writes
  // FB_HANDOFF (0xA4000000, magic "UEFIFB01") with the live scanout base; the
  // old hardcoded 0x7C610000/0x78170000 are not scanned by the panel at all,
  // which is why every previous paint stayed invisible.
  // 0x0103 = paint with live handoff geometry, 0x0106 = handoff asked but
  // missing (recorded so the next run can tell "no handoff" from "painted").
  {
    volatile UINT64  *Handoff = (volatile UINT64 *)(UINTN)0xA4000000ULL;
    UINT64            FbMagic = Handoff[0];
    UINT64            FbBase  = Handoff[1];

    TRC (0x0102, TRC_EXIT, Status, FbMagic, FbBase, 0, 0);
    if (FbMagic == 0x3130424649464555ULL && (FbBase & 0xFFF) == 0) {
      SecPaintMagenta (FbBase, 0x0103);   /* 0x0103 enter / 0x0104 written+mask */
    } else {
      TRC (0x0106, TRC_ENTER, FbMagic, FbBase, 0, 0, 0);   /* no live handoff */
      SecPaintMagenta (SEC_FB_PRIMARY, 0x0103);            /* legacy fallback */
    }
  }

  TRC (TRC_SEC_DTB, TRC_ENTER, 0, 0, 0, 0, 0);
  SecPublishBootDtb ();
  TRC (TRC_SEC_DTB, TRC_EXIT, 0, *(volatile UINT64 *)(UINTN)0xA40007F8, 0, 0, 0);

  // Build CPU HOB
  SecBeacon (0x60);
  BuildCpuHob (ArmGetPhysicalAddressBits (), PcdGet8 (PcdPrePiCpuIoSize));

  // Set the Boot Mode
  SecBeacon (0x61);
  SetBootMode (BOOT_WITH_DEFAULT_SETTINGS);

  // Initialize Platform HOBs
  SecBeacon (0x62);
  Status = PlatformPeim ();
  SecBeacon (0x63);
  if (EFI_ERROR (Status)) {
    SecBeacon (0xE3);
    return;
  }

  // Run Library Cconstructors (expanded to bisect; each ASSERT_RETURN_ERROR
  // in the generated list would CpuDeadLoop on error, so record each step).
  // Before anything walks the HOB chain, snapshot its head into the beacon
  // area: if GetFirstGuidHob() below hangs, the dump shows which HOB broke
  // the chain (zero/short length prevents GET_NEXT_HOB from advancing).
  SecDumpHobChain ();
  SecBeacon (0x64);
  {
    extern RETURN_STATUS EFIAPI BaseDebugLibSerialPortConstructor (VOID);
    extern RETURN_STATUS EFIAPI ExtractGuidedSectionLibConstructor (VOID);
    extern RETURN_STATUS EFIAPI ArmMmuBaseLibConstructor (VOID);
    extern RETURN_STATUS EFIAPI LzmaDecompressLibConstructor (VOID);
    // Walk-probe GUID: deliberately absent from the HOB chain, so the walk
    // traverses the entire list exactly like the real
    // gArmMmuReplaceLiveTranslationEntryFuncGuid lookup inside
    // ArmMmuBaseLibConstructor. Same loop, no symbol coupling.
    STATIC CONST EFI_GUID mSecHobWalkProbeGuid = {
      0x1d4a5f00, 0xbe10, 0x4ec0, { 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58 }
    };
    VOID *HobProbe;

    Status = BaseDebugLibSerialPortConstructor ();
    SecBeacon (0x70);
    Status = ExtractGuidedSectionLibConstructor ();
    SecBeacon (0x71);

    // Replicate the constructor's HOB walk with beacons around it:
    //   hang at 0x71        -> the HOB walk itself is the hang
    //   0x75 (probe NULL)   -> walk returned; hang would then be inside the
    //                          constructor's WriteBackDataCacheRange branch
    //   0x76 (probe found)  -> walk matched (unexpected for this GUID)
    SecBeacon (0x74);
    HobProbe = GetFirstGuidHob (&mSecHobWalkProbeGuid);
    SecBeacon (HobProbe == NULL ? 0x75 : 0x76);

    Status = ArmMmuBaseLibConstructor ();
    SecBeacon (0x72);
    Status = LzmaDecompressLibConstructor ();
    SecBeacon (0x73);
  }

  // Decompress all FVs
  SecBeacon (0x65);
  Status = DecompressFirstFv ();
  SecBeacon (0x66);
  SecContainerSnapshot (6);  /* T6: after FV decompress */
  if (EFI_ERROR (Status)) {
    SecBeacon (0xE6);
    DEBUG ((EFI_D_ERROR, "Failed to Decompress FV! Status = %r\n", Status));
    return;
  }

  // Load DXE Core
  SecBeacon (0x67);
  SecContainerSnapshot (7);  /* T7: just before DXE core load */
  TRC (TRC_SEC_DXE_JUMP, TRC_ENTER, 0, 0, 0, 0, 0);
  Status = LoadDxeCoreFromFv (NULL, 0);
  TRC (TRC_SEC_DXE_JUMP, TRC_EXIT, Status, 0, 0, 0, 0);
  SecBeacon (0x68);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Load DXE Core! Status = %r\n", Status));
  }
}


VOID
SecEntry (VOID)
{
  TRC (TRC_SEC_ENTRY, TRC_ENTER, 0, TrcSp (), 0, 0, 0);
  // Disable Data Cache
  ArmDisableDataCache ();

  // Invalidate Instruction Cache
  ArmInvalidateInstructionCache ();

  // Enable Instruction Cache
  ArmEnableInstructionCache ();

  // Invalidate Stack & UEFI FD D-Cache
  InvalidateDataCacheRange ((VOID *)FixedPcdGet64 (PcdCPUCoresStackBase), FixedPcdGet32 (PcdCPUCorePrimaryStackSize));
  InvalidateDataCacheRange ((VOID *)FixedPcdGet64 (PcdFdBaseAddress),     FixedPcdGet32 (PcdFdSize));

  // Verify Exception Vector Table
  ASSERT (((UINTN)SecVectorTable & ARM_VECTOR_TABLE_ALIGNMENT) == 0);

  // Enable new Exception Vector Table
  ArmWriteVBar ((UINTN)SecVectorTable);

  // Enter Main SEC Function
  SecMain ();
}
