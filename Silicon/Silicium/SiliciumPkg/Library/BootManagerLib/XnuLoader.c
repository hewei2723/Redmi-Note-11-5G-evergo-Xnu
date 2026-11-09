/**
  XNU loader for evergo, stage 0.

  Ported from hackios-pencilpen1 (device: redwood), where the boot shim copies
  the "XNUP" container to XNU_PAYLOAD_BASE and BootManagerLib starts
  "loader.efi" out of it. evergo's hot-jump path has no boot shim, so the
  container is expected in the existing no-map payload reservation instead
  (see XnuLoader.h) and is placed there by whoever prepares the jump.

  Behaviour on evergo differs from redwood in one respect: when the container
  carries no "loader.efi", the stage-0 diagnostic log is collected and this
  function RETURNS to BDS instead of cold-resetting the phone, so a beacon and
  the existing BDS path stay observable.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/EvergoTrace.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Protocol/BlockIo.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/PartitionInfo.h>

#include "XnuLoader.h"

#define LOG_SIZE  SIZE_256KB

STATIC CHAR8  *mLog;
STATIC UINTN   mLogLen;

#ifdef EVERGO_SEC_BEACONS
//
// Same reserved ramoops slot the SEC and DxeMain/BdsEntry beacons use. This
// library is linked into BdsDxe, whose copy of DxeBeacon is static, so a small
// duplicate keeps the loader self-contained.
//
STATIC
VOID
XnuBeacon (
  IN UINTN  Step
  )
{
  volatile UINT64 *Beacon = (volatile UINT64 *)0x4811FC00ULL;
  volatile UINT64 *Mirror = (volatile UINT64 *)0xB6BE0000ULL;
  volatile UINT32 *Wdt    = (volatile UINT32 *)0x10007000ULL;
  UINTN            Line;

  Beacon[0] = 0x53473142ULL;
  Beacon[1] = (UINT64)Step;
  Mirror[0] = 0x53473142ULL;
  Mirror[1] = (UINT64)Step;
  asm volatile ("dsb sy" ::: "memory");
  Wdt[2] = 0x1971;
  asm volatile ("dsb sy" ::: "memory");
  for (Line = 0; Line < 256; Line += 64) {
    asm volatile ("dc cvac, %0" :: "r" (0x4811FC00ULL + Line));
  }
  asm volatile ("dsb sy" ::: "memory");
}
#define XNU_BEACON(Step)  XnuBeacon (Step)
#else
#define XNU_BEACON(Step)  do { } while (0)
#endif

//
// Minimal writer for the same persisted-log format the kernel module reads:
// magic "XLOG" | length | commit 0xA5A5A5A5 | pad | text. Used only when this
// firmware is the last thing that can report something (for example a container
// without a "loader.efi" entry), because loader.efi overwrites this area with
// its own, much longer log when it does run.
//
#define XNU_PERSIST_BASE    0xB6BF0000ULL
#define XNU_PERSIST_SIZE    0x10000ULL
#define XNU_PERSIST_MAGIC   0x474F4C58U   /* "XLOG" */
#define XNU_PERSIST_COMMIT  0xA5A5A5A5U

STATIC
VOID
XnuPersistLog (
  IN CONST CHAR8  *Text
  )
{
  volatile UINT32  *Header = (volatile UINT32 *)(UINTN)XNU_PERSIST_BASE;
  volatile CHAR8   *Body   = (volatile CHAR8 *)(UINTN)(XNU_PERSIST_BASE + 16);
  UINTN             Length = 0;
  UINTN             Limit  = (UINTN)(XNU_PERSIST_SIZE - 16);

  while ((Text[Length] != 0) && (Length < Limit)) {
    Length++;
  }

  Header[2] = 0;
  Header[0] = XNU_PERSIST_MAGIC;
  Header[1] = (UINT32)Length;
  for (UINTN Index = 0; Index < Length; Index++) {
    Body[Index] = Text[Index];
  }

  asm volatile ("dsb sy" ::: "memory");
  Header[2] = XNU_PERSIST_COMMIT;
  asm volatile ("dsb sy" ::: "memory");
}

#ifdef EVERGO_SEC_BEACONS
//
// Durable loader-stage code in beacon slot 17, bits 32..39. The step in slot 1
// is overwritten by the BDS beacons that follow, so this is the only way to
// tell afterwards which return path the loader took.
//
#define XNU_STAGE_NO_CONTAINER   1u
#define XNU_STAGE_ENTERED        2u
#define XNU_STAGE_LOADIMAGE_FAIL 3u
#define XNU_STAGE_STARTIMAGE_FAIL 4u
#define XNU_STAGE_RETURNED_OK    5u
#define XNU_STAGE_NO_LOADER_EFI  6u

STATIC
VOID
XnuStageMark (
  IN UINT32  Stage
  )
{
  volatile UINT64  *Slot = (volatile UINT64 *)(UINTN)(0xB6BE0000ULL + 17 * 8);

  *Slot = (*Slot & ~0x000000FF00000000ULL) | ((UINT64)Stage << 32);
  asm volatile ("dsb sy" ::: "memory");
}
#else
#define XnuStageMark(Stage)  do { } while (0)
#endif

STATIC
VOID
EFIAPI
Log (
  IN CONST CHAR8  *Format,
  ...
  )
{
  VA_LIST  Marker;

  if ((mLog == NULL) || (mLogLen >= LOG_SIZE - 512)) {
    return;
  }

  VA_START (Marker, Format);
  mLogLen += AsciiVSPrint (mLog + mLogLen, LOG_SIZE - mLogLen, Format, Marker);
  VA_END (Marker);
}

#define READ_SYSREG(Name) ({ UINT64 _Value; __asm__ volatile ("mrs %0, " #Name : "=r" (_Value)); _Value; })

STATIC
EFI_BLOCK_IO_PROTOCOL *
FindPartition (
  IN CONST CHAR16  *Name
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *Handles;
  UINTN       Count;

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiPartitionInfoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    return NULL;
  }

  for (UINTN Index = 0; Index < Count; Index++) {
    EFI_PARTITION_INFO_PROTOCOL  *Info;
    EFI_BLOCK_IO_PROTOCOL        *BlockIo;

    Status = gBS->HandleProtocol (Handles[Index], &gEfiPartitionInfoProtocolGuid, (VOID **)&Info);
    if (EFI_ERROR (Status) || (Info->Type != PARTITION_TYPE_GPT)) {
      continue;
    }

    if (StrnCmp (Info->Info.Gpt.PartitionName, Name, 36) != 0) {
      continue;
    }

    Status = gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo);
    if (!EFI_ERROR (Status)) {
      return BlockIo;
    }
  }

  return NULL;
}

STATIC
VOID
LogCpu (
  VOID
  )
{
  Log ("[cpu]\n");
  Log ("CurrentEL=%lx\n", READ_SYSREG (CurrentEL) >> 2);
  Log ("MIDR_EL1=%016lx\n", READ_SYSREG (MIDR_EL1));
  Log ("ID_AA64MMFR0_EL1=%016lx\n", READ_SYSREG (ID_AA64MMFR0_EL1));
  Log ("ID_AA64MMFR1_EL1=%016lx\n", READ_SYSREG (ID_AA64MMFR1_EL1));
  Log ("ID_AA64MMFR2_EL1=%016lx\n", READ_SYSREG (ID_AA64MMFR2_EL1));
  Log ("ID_AA64PFR0_EL1=%016lx\n", READ_SYSREG (ID_AA64PFR0_EL1));
  Log ("ID_AA64PFR1_EL1=%016lx\n", READ_SYSREG (ID_AA64PFR1_EL1));
  Log ("ID_AA64ISAR0_EL1=%016lx\n", READ_SYSREG (ID_AA64ISAR0_EL1));
  Log ("ID_AA64ISAR1_EL1=%016lx\n", READ_SYSREG (ID_AA64ISAR1_EL1));
  Log ("ID_AA64DFR0_EL1=%016lx\n", READ_SYSREG (ID_AA64DFR0_EL1));
  Log ("SCTLR_EL1=%016lx\n", READ_SYSREG (SCTLR_EL1));
  Log ("TCR_EL1=%016lx\n", READ_SYSREG (TCR_EL1));
  Log ("MAIR_EL1=%016lx\n", READ_SYSREG (MAIR_EL1));
  Log ("TTBR0_EL1=%016lx\n", READ_SYSREG (TTBR0_EL1));
  Log ("VBAR_EL1=%016lx\n", READ_SYSREG (VBAR_EL1));
  Log ("CNTFRQ_EL0=%ld\n", READ_SYSREG (CNTFRQ_EL0));
  Log ("MPIDR_EL1=%016lx\n", READ_SYSREG (MPIDR_EL1));
  Log ("DAIF=%lx\n", READ_SYSREG (DAIF));
}

STATIC
VOID
LogDisplay (
  VOID
  )
{
  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;

  Log ("[display]\n");

  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  if (EFI_ERROR (Status)) {
    Log ("gop=missing status=%r\n", Status);
    return;
  }

  Log ("fb_base=%lx fb_size=%lx\n", Gop->Mode->FrameBufferBase, (UINT64)Gop->Mode->FrameBufferSize);
  Log (
       "width=%d height=%d stride=%d format=%d\n",
       Gop->Mode->Info->HorizontalResolution,
       Gop->Mode->Info->VerticalResolution,
       Gop->Mode->Info->PixelsPerScanLine,
       Gop->Mode->Info->PixelFormat
       );
}

STATIC
VOID
LogMemoryMap (
  VOID
  )
{
  EFI_STATUS             Status;
  EFI_MEMORY_DESCRIPTOR  *Map;
  UINTN                  MapSize = 0;
  UINTN                  MapKey;
  UINTN                  DescSize;
  UINT32                 DescVersion;

  Log ("[memmap]\n");

  Status = gBS->GetMemoryMap (&MapSize, NULL, &MapKey, &DescSize, &DescVersion);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    Log ("getmemorymap status=%r\n", Status);
    return;
  }

  MapSize += SIZE_4KB;
  Map      = AllocatePool (MapSize);
  if (Map == NULL) {
    return;
  }

  Status = gBS->GetMemoryMap (&MapSize, Map, &MapKey, &DescSize, &DescVersion);
  if (EFI_ERROR (Status)) {
    Log ("getmemorymap status=%r\n", Status);
    return;
  }

  for (UINTN Offset = 0; Offset < MapSize; Offset += DescSize) {
    EFI_MEMORY_DESCRIPTOR  *Desc = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Offset);

    Log (
         "%016lx %016lx type=%d attr=%lx\n",
         Desc->PhysicalStart,
         Desc->PhysicalStart + EFI_PAGES_TO_SIZE (Desc->NumberOfPages),
         Desc->Type,
         Desc->Attribute
         );
  }

  FreePool (Map);
}

STATIC
VOID
LogPayload (
  VOID
  )
{
  XNU_PAYLOAD_HEADER  *Header = (XNU_PAYLOAD_HEADER *)(UINTN)XNU_PAYLOAD_BASE;

  Log ("[payload]\n");
  Log ("base=%lx magic=%08x size=%x\n", (UINT64)XNU_PAYLOAD_BASE, Header->Magic, Header->Size);
}

STATIC
VOID
LogPersistence (
  VOID
  )
{
  //
  // Does RAM written before a reset survive it? Uses the tail of the payload
  // window; on evergo that is the 4 KiB below the DXE heap base.
  //
  UINT32  *Ram = (UINT32 *)(UINTN)(XNU_PAYLOAD_BASE + XNU_PAYLOAD_MAX_SIZE - SIZE_4KB);

  Log ("[persistence]\n");
  Log ("ram_before=%08x %08x\n", Ram[0], Ram[1]);

  Ram[1] = (Ram[0] == XNU_MARKER_MAGIC) ? Ram[1] + 1 : 1;
  Ram[0] = XNU_MARKER_MAGIC;

  Log ("ram_after=%08x %08x\n", Ram[0], Ram[1]);
}

STATIC
VOID
FlushLog (
  VOID
  )
{
  EFI_STATUS             Status;
  EFI_BLOCK_IO_PROTOCOL  *BlockIo;
  UINTN                  BlockSize;
  UINTN                  WriteSize;

  BlockIo = FindPartition (XNU_LOG_PARTITION);
  if (BlockIo == NULL) {
    return;
  }

  BlockSize = BlockIo->Media->BlockSize;
  WriteSize = ALIGN_VALUE (mLogLen + 1, BlockSize);

  Status = BlockIo->WriteBlocks (BlockIo, BlockIo->Media->MediaId, 0, WriteSize, mLog);
  if (!EFI_ERROR (Status)) {
    BlockIo->FlushBlocks (BlockIo);
  }
}

//
// The payload is a list of named blobs. If it carries "loader.efi", that is
// the real XNU loader: start it straight from memory, so the loader can be
// iterated on without rebuilding this firmware.
//
STATIC
VOID
StartPayloadLoader (
  VOID
  )
{
  XNU_PAYLOAD_HEADER  *Header = (XNU_PAYLOAD_HEADER *)(UINTN)XNU_PAYLOAD_BASE;
  EFI_STATUS           Status;
  UINT8               *Cursor;
  UINT8               *End;

  if ((Header->Magic != XNU_PAYLOAD_MAGIC) || (Header->Size > XNU_PAYLOAD_MAX_SIZE)) {
    return;
  }

  Cursor = (UINT8 *)(Header + 1);
  End    = Cursor + Header->Size;

  while (Cursor + sizeof (XNU_PAYLOAD_ENTRY) <= End) {
    XNU_PAYLOAD_ENTRY  *Entry = (XNU_PAYLOAD_ENTRY *)Cursor;

    if (Entry->Name[0] == 0) {
      break;
    }

    if (AsciiStrnCmp (Entry->Name, "loader.efi", sizeof (Entry->Name)) == 0) {
      EFI_HANDLE  Handle;

      //
      // Record the memory descriptor that covers the image's preferred load
      // base (0x41000000 for this loader) BEFORE LoadImage. If the type there
      // is not EfiConventionalMemory, DXE's preferred-base AllocateAddress is
      // guaranteed to fail and we want that visible in the beacon, not guessed.
      //
      {
        UINTN                  MSize = 0, MKey = 0, DSize = 0;
        UINT32                 DVer = 0;
        EFI_MEMORY_DESCRIPTOR  *MMap;
        UINT64                  PrefBase = 0x41000000ULL;
        UINT64                  Type = 0xFFFF, Base = 0, Pages = 0;

        if (gBS->GetMemoryMap (&MSize, NULL, &MKey, &DSize, &DVer) == EFI_BUFFER_TOO_SMALL) {
          MSize += SIZE_4KB;
          MMap = AllocatePool (MSize);
          if (MMap != NULL) {
            if (!EFI_ERROR (gBS->GetMemoryMap (&MSize, MMap, &MKey, &DSize, &DVer))) {
              for (UINTN O = 0; O + DSize <= MSize; O += DSize) {
                EFI_MEMORY_DESCRIPTOR *D = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)MMap + O);
                if ((D->PhysicalStart <= PrefBase) &&
                    (D->PhysicalStart + EFI_PAGES_TO_SIZE (D->NumberOfPages) > PrefBase)) {
                  Type  = (UINT64)D->Type;
                  Base  = D->PhysicalStart;
                  Pages = D->NumberOfPages;
                  break;
                }
              }
            }
            FreePool (MMap);
          }
        }
        *(volatile UINT64 *)(UINTN)(0x4811FC00ULL + 2 * 8) = Type;   /* descriptor type at 0x41000000 */
        *(volatile UINT64 *)(UINTN)(0x4811FC00ULL + 3 * 8) = Base;
        *(volatile UINT64 *)(UINTN)(0x4811FC00ULL + 4 * 8) = Pages;
        asm volatile ("dsb sy" ::: "memory");
      }

      TRC (TRC_LOADER_LOADIMG, TRC_ENTER, 0, Entry->Size, (UINT64)(UINTN)(Entry + 1), 0, 0);
      Status = gBS->LoadImage (FALSE, gImageHandle, NULL, Entry + 1, Entry->Size, &Handle);
      TRC (TRC_LOADER_LOADIMG, TRC_EXIT, Status, (UINT64)Handle, 0, 0, 0);
      if (EFI_ERROR (Status)) {
        //
        // The loader carries ~6 MiB of BSS, so this is where a small DXE heap
        // shows up first. 0xB0 makes that distinguishable from a silent return.
        //
        XNU_BEACON (0xB0);
        XnuStageMark (XNU_STAGE_LOADIMAGE_FAIL);
        DEBUG ((DEBUG_ERROR, "XnuLoader: LoadImage %r\n", Status));
        return;
      }

      *(volatile UINT64 *)(UINTN)(0xB6BE0000ULL + 14 * 8) = 0xF0;   /* about to start */
      asm volatile ("dsb sy" ::: "memory");
      TRC (TRC_LOADER_STARTIMG, TRC_ENTER, 0, (UINT64)Handle, 0, 0, 0);
      Status = gBS->StartImage (Handle, NULL, NULL);
      TRC (TRC_LOADER_STARTIMG, TRC_EXIT, Status, (UINT64)Handle, 0, 0, 0);
      *(volatile UINT64 *)(UINTN)(0xB6BE0000ULL + 14 * 8) = 0xF1;   /* StartImage returned */
      asm volatile ("dsb sy" ::: "memory");
      if (EFI_ERROR (Status)) {
        XNU_BEACON (0xB1);
        XnuStageMark (XNU_STAGE_STARTIMAGE_FAIL);
        DEBUG ((DEBUG_ERROR, "XnuLoader: StartImage %r\n", Status));
      } else {
        XNU_BEACON (0xB2);
        XnuStageMark (XNU_STAGE_RETURNED_OK);
      }

      return;
    }

    Cursor += sizeof (XNU_PAYLOAD_ENTRY) + ALIGN_VALUE (Entry->Size, 16);
  }

  //
  // Valid container, but it carries no "loader.efi": nothing to start.
  //
  XNU_BEACON (0xB3);
  XnuStageMark (XNU_STAGE_NO_LOADER_EFI);
  XnuPersistLog ("XnuLoader: container has no loader.efi entry\n");
}

BOOLEAN
EFIAPI
XnuLoaderPayloadPresent (
  VOID
  )
{
  XNU_PAYLOAD_HEADER  *Header = (XNU_PAYLOAD_HEADER *)(UINTN)XNU_PAYLOAD_BASE;

  return (BOOLEAN)((Header->Magic == XNU_PAYLOAD_MAGIC) &&
                   (Header->Size != 0) &&
                   (Header->Size <= XNU_PAYLOAD_MAX_SIZE));
}

VOID
EFIAPI
XnuLoaderMain (
  VOID
  )
{
  EFI_TIME  Time;

  if (!XnuLoaderPayloadPresent ()) {
    //
    // Reached BDS but no container: BootShim did not copy one (or copied it
    // somewhere else). Distinguishable from "container present but unusable".
    //
    XNU_BEACON (0xAD);
    XnuStageMark (XNU_STAGE_NO_CONTAINER);
    XnuPersistLog ("XnuLoader: no XNUP container at 0xB5400000\n");
    return;
  }

  XNU_BEACON (0xAE);
  XnuStageMark (XNU_STAGE_ENTERED);
  TRC (TRC_LOADER_MAIN, TRC_ENTER, 0, 0, 0, 0, 0);
  //
  // The panel is the only output channel on the hot-jump path (no serial).
  // DEBUG() reaches the UEFI console, so these marks are visible on screen
  // and make "where did the loader stop" answerable from a photo.
  //
  DEBUG ((DEBUG_ERROR, "XNU: main enter\n"));
  XNU_BEACON (0xB0);

  //
  // The payload window is NOT pinned here, on purpose.
  //
  // Pinning meant gBS->AllocatePages(AllocateAddress, EfiLoaderData, ...) over
  // this region. The region descriptor type is EfiBootServicesData, and Mu's
  // CoreConvertPages refuses non-conventional -> non-conventional conversions,
  // so the call always failed - and worse, CoreInternalAllocatePages calls
  // PromoteMemoryResource() while still holding the memory lock on that failure
  // path, which deadlocks (bisected 2026-11-07: beacon stopped right at the pin
  // block and the watchdog reset the device).
  //
  // The pin is also unnecessary: because the container stays a non-conventional
  // (allocated) descriptor, the loader's AllocateAnyPages(LoaderData) can only
  // pick EfiConventionalMemory ranges, so it can never choose this window as
  // its kernel destination.
  //
  XNU_BEACON (0xB1);          /* pin deliberately skipped (see above) */

  //
  // Record how much conventional memory DXE still has, in MiB, in beacon
  // slot 15. The loader image needs a multi-MiB contiguous allocation, and a
  // failed LoadImage is otherwise indistinguishable from a silent return.
  //
  {
    UINTN                  MapSize = 0;
    UINTN                  MapKey;
    UINTN                  DescSize;
    UINT32                 DescVersion;
    EFI_MEMORY_DESCRIPTOR  *Map;
    UINT64                 FreeMiB = 0;

    if (gBS->GetMemoryMap (&MapSize, NULL, &MapKey, &DescSize, &DescVersion) == EFI_BUFFER_TOO_SMALL) {
      MapSize += SIZE_4KB;
      Map = AllocatePool (MapSize);
      if (Map != NULL) {
        if (!EFI_ERROR (gBS->GetMemoryMap (&MapSize, Map, &MapKey, &DescSize, &DescVersion))) {
          for (UINTN Off = 0; Off + DescSize <= MapSize; Off += DescSize) {
            EFI_MEMORY_DESCRIPTOR *Desc = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Off);
            if (Desc->Type == EfiConventionalMemory) {
              FreeMiB += EFI_PAGES_TO_SIZE (Desc->NumberOfPages);
            }
          }
          FreeMiB >>= 20;
          TRC (TRC_LOADER_FREE, TRC_EXIT, 0, FreeMiB, 0, 0, 0);
          XNU_BEACON (0xB3);
          *(volatile UINT64 *)(UINTN)(0xB6BE0000ULL + 15 * 8) = FreeMiB;
          asm volatile ("dsb sy" ::: "memory");
        }
        FreePool (Map);
      }
    }
  }

  XNU_BEACON (0xB4);          /* entering StartPayloadLoader (no console I/O here) */
  // Normally does not return when the container carries loader.efi.
  StartPayloadLoader ();
  XNU_BEACON (0xB5);          /* StartPayloadLoader returned */

  mLog = AllocateZeroPool (LOG_SIZE);
  if (mLog != NULL) {
    Log ("== XNULOADER LOG v1 (evergo) ==\n");

    if (!EFI_ERROR (gRT->GetTime (&Time, NULL))) {
      Log ("time=%04d-%02d-%02d %02d:%02d:%02d\n", Time.Year, Time.Month, Time.Day, Time.Hour, Time.Minute, Time.Second);
    }

    FlushLog ();
    LogCpu ();
    FlushLog ();
    LogDisplay ();
    FlushLog ();
    LogMemoryMap ();
    FlushLog ();
    LogPayload ();
    FlushLog ();
    LogPersistence ();
    Log ("== END ==\n");
    FlushLog ();
  }

  //
  // evergo difference: do NOT cold-reset. Returning keeps the BDS beacons and
  // the boot-option path observable; the caller decides what happens next.
  //
  XNU_BEACON (0xAF);
}
