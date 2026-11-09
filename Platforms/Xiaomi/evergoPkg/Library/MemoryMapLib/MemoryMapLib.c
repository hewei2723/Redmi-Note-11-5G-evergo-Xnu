#include <Library/MemoryMapLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>

/*
 * evergo (Redmi Note 11 5G, MT6833P) memory map for the *cold-boot* UEFI path:
 *   LK loads boot.img kernel = BootShim + evergo.fd at 0x40080000,
 *   ramdisk (our FAT image with loader.efi + boot.xnup) at 0x51100000,
 *   DTB at 0x47C80000.
 *
 * Provenance per region is in EVERGO-BOOT-ARCHITECTURE-REVIEW.md §4.
 * "[DT]" = evergo's own reserved-memory / FDT; "[LK]" = stock boot.img geometry;
 * "[camellia]" = same-SoC MT6833 UEFI reference, not yet verified on evergo.
 *
 * The hot-jump-only regions of the camellia/lancelot templates (0x98000000 payload,
 * 0x9C000000 scanout, 0xA4000000 FB handoff, XNU fixed window) are intentionally
 * removed: the cold-boot path carries its payload in the boot.img ramdisk.
 */
STATIC
EFI_MEMORY_REGION_DESCRIPTOR
gMemoryDescriptor[] = {
  // Name, Address, Length, HobOption, ResourceType, ResourceAttribute, MemoryType, ArmAttribute

  // ---- UEFI / boot chain ----
  // HOT-JUMP layout: the FD is copied to 0xB0000000 and the stack PCD is
  // 0xB0200000 (both inside the DTB no-map slot uefi-fd@B0000000). The MMU tables
  // built from this table MUST cover the real stack, or the first stack
  // access after ArmConfigureMmu faults.
  // NOTE: these regions MUST NOT overlap. ValidateMemoryMap() sorts by
  // base address and calls CpuDeadLoop() on any adjacent overlap, which is a
  // silent hang because the UART is disabled. The FD is 2 MiB and ends exactly
  // where the stack begins.
  {"UEFI Stack",         0xB0200000, 0x00040000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"Evergo Trace",        0xB0300000, 0x00010000, AddMem, Reserv, SYS_MEM_CAP, BsData, WRITE_BACK},// TRC1 ring; inside the 4 MiB FD reservation, proven writable from both sides
  {"UEFI FD",            0xB0000000, 0x00200000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},   // 2 MiB FD; stack at 0xB0200000 inside the 4 MiB DTB no-map slot
  // The DXE heap is ALSO the HOB list / page-table / allocation arena
  // (Sec.c: HobConstructor(heap, heap+size); ArmConfigureMmu allocates the
  // translation tables from it). It previously sat at 0x40280000, i.e. inside
  // the running kernel's System RAM (Kernel code 0x40080000-0x41f7ffff,
  // Kernel data 0x42790000-0x4329afff on 2026-11-05) - live kernel memory.
  // It now lives inside the EXISTING no-map DTB reservation
  // xnup-payload@B5400000 (reg = 0xb5400000 + 0x4000000), which Linux already
  // keeps out of System RAM, so no boot-image change is required. Only the
  // 16 MiB TAIL of that window is used, so the first 48 MiB
  // [0xb5400000,0xb8400000) stays free for the XNU payload image that
  // CamelliaUniXNULoaderDxe expects to publish at 0xA4000780 with
  // RAM_PARTITION_LBA=2048 (payload partition = Base + 1 MiB).
  // Budget: 55 FFS images 3.6 MiB + decompressed FVMAIN 3.16 MiB + DxeCore
  // + pools/stacks/page tables < 9 MiB, so 16 MiB has room but not much slack.
  // Change this only together with preflight_testd.sh and the stage1
  // cache-maintenance range.
  // XNUP payload window (XnuLoader.h: XNU_PAYLOAD_BASE). Reserved (not SYS_MEM)
  // so DXE never allocates inside it: the container and the beacon mirror
  // at 0xB6BE0000 must survive until the loader reads them.
  // BootShim copies the
  // container here before the jump, so UEFI must be able to READ it: without
  // this entry the first access in XnuLoaderPayloadPresent() takes a
  // translation fault and the boot dies silently right after AfterConsole.
  {"XNUP Payload",       0xB5400000, 0x01800000, AddMem, Reserv,  SYS_MEM_CAP, BsData, WRITE_BACK},
  // Trace copy C: FB handoff page immediately AFTER the DTB pointer slot
  // 0xA40007F8; the handoff magic (0xA4000000) and the published DTB stay
  // untouched, the records live at 0xA4000800..0xA4001000 inside the same
  // AddDev MmIO page.
  {"FB Handoff",         0xA4000000, 0x00100000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"DXE Heap",           0xB6C00000, 0x02800000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
  {"Boot DTB",           0x47C80000, 0x00030000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},   // [LK] tags/dtb 0x47C80000
  {"Boot Ramdisk",       0x51100000, 0x04000000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},   // [LK] ramdisk 0x51100000 (FAT + XNUP, <=64 MiB)

  // ---- evergo reserved memory from its own DTB / /proc/cmdline ----
  {"AEE Debug Kinfo",    0x48080000, 0x00010000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},   // [DT] mblock-17
  {"pstore / ramoops",   0x48090000, 0x000E0000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},   // [DT] mblock-18 + cmdline
  {"minirdump",          0x48170000, 0x00010000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},   // [DT] mblock-19
  {"ATF Reserved",       0x48C00000, 0x00200000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},   // [DT] mblock-7
  {"AP MD C SMEM",       0x6C000000, 0x02960000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},   // [DT] mblock-31
  {"TEE Reserved",       0x70000000, 0x05B00000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},   // [DT] mblock-14
  {"SPM Reserved",       0x77FF0000, 0x00010000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},   // [DT] mblock-21
  {"AP MD NC SMEM",      0x7A000000, 0x001B0000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},   // [DT] mblock-30
  {"LK Framebuffer",     0x78170000, 0x01BE0000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH},// [DT /chosen atag,videolfb cell[3]] evergo LK fb base, vram 0x1be0000
  {"Android Framebuffer",0x7C610000, 0x01DF0000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH},// [DT mblock-20]
  {"TEE REE",            0x7E400000, 0x00C00000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},   // [DT] mblock-13
  {"TEE SecMem",         0x7F000000, 0x00400000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},   // [DT] mblock-10
  {"GZ Log",             0x7F400000, 0x00041000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},   // [DT] mblock-5
  {"MCUPM Reserved",     0x7F500000, 0x00100000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},   // [DT] mblock-15
  {"GZ",                 0x7F600000, 0x00800000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},   // [DT] mblock-4
  {"SSPM Reserved",      0x7FEB0000, 0x00100000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},   // [DT] mblock-11
  {"log_store",          0x7FFBF000, 0x00040000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},   // [DT] mblock-6
  {"EMI MBIST",          0x7FFFF000, 0x00001000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},   // [DT] mblock-3
  {"DRDI Memory",        0x9A090000, 0x00480000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},   // [DT] mblock-29
  {"MD Mem Usage",       0x9C000000, 0x079B0000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},   // [DT] mblock-27
  // FrameBufferSerialPortLib reads the handoff magic at 0xA4000000 on every
  // DEBUG print. It MUST be mapped, or the first SerialPortWrite after
  // ArmConfigureMmu faults on an unmapped address and hangs (0xA4000000 is
  // Linux System RAM, so an unmatched stale-magic read is harmless and ignored).
  {"MD Mem Usage 2",     0xB3BD0000, 0x01730000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},   // [DT] mblock-28
  {"SCP Reserved",       0xB9400000, 0x00400000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},   // [DT] mblock-22
  {"TEE Log",            0xBFC00000, 0x00020000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},   // [DT] mblock-12
  {"PICACHU",            0xBFD00000, 0x00080000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},   // [DT] mblock-16
  {"ATF Log",            0xBFE00000, 0x00040000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},   // [DT] mblock-9
  {"CCCI Tag Mem",       0xBFF70000, 0x00010000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},   // [DT] mblock-23
  {"ATF Ramdump",        0xBFF80000, 0x00080000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},   // [DT] mblock-8
  {"DPM Reserved",       0xFFE60000, 0x00190000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},   // [DT] mblock-32

  // ---- High DDR (DPU scanout / GPU / modem) ----
  // The panel's real scanout buffer is dynamically allocated ABOVE 4 GiB on
  // this 8 GiB device; observed bases span 0x116ae8000 .. 0x205c98000, so a
  // narrow window is not enough. SEC and any UEFI blit must be able to
  // translate the whole high-DDR range, otherwise the write takes an L1
  // translation fault (ESR 0x96000045) and the FD exception-loops black.
  {"High DDR Framebuffer", 0x100000000, 0x140000000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH},

  // ---- Register Regions ----
  {"GIC Distributor",    0x0C000000, 0x00040000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"GIC Redistributors", 0x0C040000, 0x00200000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"Top Ck Gen",         0x10000000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"Infra Cfg AO",       0x10001000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"Pinctrl",            0x10005000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"SPM Control",        0x10006000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"WatchDog Timer",     0x10007000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"AP Mixed",           0x1000C000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"PMIC Wrapper",       0x1000D000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"EMI",                0x10219000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"USB2 PHY",           0x11CC0000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"MUSB MAC",           0x11200000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"UFSHCI",             0x11270000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"UFS M-PHY",          0x11FA0000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"MFG Configuration",  0x13000000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"Mali Registers",     0x13040000, 0x00004000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"DISP Config",        0x14000000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"DISP OVL0",          0x14005000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"DISP OVL0 2L",       0x14006000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"DISP RDMA0",         0x14007000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"DSI0",               0x14013000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE}
};

VOID
GetMemoryMap (
  OUT EFI_MEMORY_REGION_DESCRIPTOR **MemoryDescriptor,
  OUT UINT8                         *MemoryDescriptorCount
)
{
  *MemoryDescriptor      = gMemoryDescriptor;
  *MemoryDescriptorCount = ARRAY_SIZE (gMemoryDescriptor);
}
