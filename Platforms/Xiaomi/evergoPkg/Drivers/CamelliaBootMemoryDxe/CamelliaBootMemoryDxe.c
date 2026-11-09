/** @file
  Publish the boot.img ramdisk and LK framebuffer described by the DTB.
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/FdtLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/DiskIo.h>
#include <Library/EvergoTrace.h>

/* Flags-word contract, mirrors FrameBufferMap.h (this driver keeps local
 * copies of the handoff structures and has no SiliciumPkg include path). */
#define FB_MAP_FLAGS_OFFSET  0x658U
#define FB_MAP_FLAG_FBDC     (1U << 0)

#define DTB_POINTER_ADDR    0xA40007F8ULL
#define FB_HANDOFF_ADDR     0xA4000000ULL
#define FB_PAGE_MAP_ADDR    0xA4000100ULL
#define FB_HANDOFF_MAGIC    0x3130424649464555ULL
#define FB_PAGE_MAP_MAGIC   0x314D504649465545ULL
#define FB_WIDTH            1088U
#define FB_HEIGHT           2400U
#define FB_PITCH            4352U
#define FB_DEPTH            32U
#define RAM_BLOCK_SIZE      512U
// Measured on the running device (fb0: 1088x7200 virtual, stride 4352, 32 bpp;
// DT mblock-20-framebuffer = 0x7C610000 + 0x1DF0000). Used when the hot-jump
// path has no bootloader DTB to read /chosen/atag,videolfb from.
#define FB_BASE_EVERGO      0x7C610000ULL   /* mblock-20, what Android scans after boot */
#define FB_BASE_LK          0x78170000ULL   /* "LK Framebuffer", what LK painted into */

#pragma pack(1)
typedef struct {
  UINT64 Magic;
  UINT64 Base;
  UINT32 Width;
  UINT32 Height;
  UINT32 Pitch;
  UINT32 Depth;
} FB_HANDOFF;

typedef struct {
  UINT64 Physical;
  UINT32 Pages;
  UINT32 Reserved;
} FB_RUN;

typedef struct {
  UINT64 Magic;
  UINT64 LogicalSize;
  UINT32 PageSize;
  UINT32 RunCount;
  FB_RUN Runs[1];
} FB_RUN_MAP;

typedef struct {
  VENDOR_DEVICE_PATH        Vendor;
  HARDDRIVE_DEVICE_PATH     Partition;
  EFI_DEVICE_PATH_PROTOCOL  End;
} RAM_BLOCK_DEVICE_PATH;
#pragma pack()

STATIC RAM_BLOCK_DEVICE_PATH mDevicePath = {
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      {
        (UINT8)sizeof (VENDOR_DEVICE_PATH),
        (UINT8)(sizeof (VENDOR_DEVICE_PATH) >> 8)
      }
    },
    {
      0x45C3EAB9,
      0x7D48,
      0x4AD9,
      { 0x83, 0x03, 0xAB, 0x62, 0x15, 0xF1, 0x46, 0x10 }
    }
  },
  {
    {
      MEDIA_DEVICE_PATH,
      MEDIA_HARDDRIVE_DP,
      {
        (UINT8)sizeof (HARDDRIVE_DEVICE_PATH),
        (UINT8)(sizeof (HARDDRIVE_DEVICE_PATH) >> 8)
      }
    },
    1,
    0,
    0,
    { 0 },
    MBR_TYPE_PCAT,
    NO_DISK_SIGNATURE
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 }
  }
};

STATIC UINT8              *mRamBase;
STATIC UINT64              mRamSize;
STATIC EFI_BLOCK_IO_MEDIA  mMedia;

STATIC
EFI_STATUS
EFIAPI
RamBlockReset (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN BOOLEAN                ExtendedVerification
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockRead (
  IN  EFI_BLOCK_IO_PROTOCOL  *This,
  IN  UINT32                 MediaId,
  IN  EFI_LBA                Lba,
  IN  UINTN                  BufferSize,
  OUT VOID                   *Buffer
  )
{
  if ((This == NULL) || (Buffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  if (MediaId != mMedia.MediaId) {
    return EFI_MEDIA_CHANGED;
  }
  if ((BufferSize == 0) || ((BufferSize % RAM_BLOCK_SIZE) != 0) ||
      (Lba > mMedia.LastBlock) ||
      ((BufferSize / RAM_BLOCK_SIZE) > (mMedia.LastBlock + 1 - Lba))) {
    return EFI_INVALID_PARAMETER;
  }

  CopyMem (Buffer, mRamBase + Lba * RAM_BLOCK_SIZE, BufferSize);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockWrite (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  IN VOID                   *Buffer
  )
{
  return EFI_WRITE_PROTECTED;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockFlush (
  IN EFI_BLOCK_IO_PROTOCOL  *This
  )
{
  return EFI_SUCCESS;
}

STATIC EFI_BLOCK_IO_PROTOCOL mBlockIo = {
  EFI_BLOCK_IO_PROTOCOL_REVISION,
  &mMedia,
  RamBlockReset,
  RamBlockRead,
  RamBlockWrite,
  RamBlockFlush
};

STATIC
EFI_STATUS
EFIAPI
RamDiskRead (
  IN  EFI_DISK_IO_PROTOCOL  *This,
  IN  UINT32                MediaId,
  IN  UINT64                Offset,
  IN  UINTN                 BufferSize,
  OUT VOID                  *Buffer
  )
{
  if ((This == NULL) || (Buffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  if (MediaId != mMedia.MediaId) {
    return EFI_MEDIA_CHANGED;
  }
  if ((Offset > mRamSize) || (BufferSize > mRamSize - Offset)) {
    return EFI_INVALID_PARAMETER;
  }

  CopyMem (Buffer, mRamBase + (UINTN)Offset, BufferSize);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RamDiskWrite (
  IN EFI_DISK_IO_PROTOCOL  *This,
  IN UINT32                MediaId,
  IN UINT64                Offset,
  IN UINTN                 BufferSize,
  IN VOID                  *Buffer
  )
{
  return EFI_WRITE_PROTECTED;
}

STATIC EFI_DISK_IO_PROTOCOL mDiskIo = {
  EFI_DISK_IO_PROTOCOL_REVISION,
  RamDiskRead,
  RamDiskWrite
};

STATIC
VOID
AddMemory (
  IN EFI_PHYSICAL_ADDRESS  Base,
  IN UINT64                Size
  )
{
  EFI_STATUS                       Status;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Descriptor;

  Status = gDS->GetMemorySpaceDescriptor (Base, &Descriptor);
  if (EFI_ERROR (Status)) {
    Status = gDS->AddMemorySpace (
                    EfiGcdMemoryTypeSystemMemory,
                    Base,
                    (UINTN)Size,
                    EFI_MEMORY_WB | EFI_MEMORY_WT | EFI_MEMORY_WC | EFI_MEMORY_UC
                    );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "CAMBOOT: add memory 0x%lx+0x%lx failed %r\n",
              Base, Size, Status));
      return;
    }
  }
  Status = gDS->SetMemorySpaceAttributes (Base, (UINTN)Size, EFI_MEMORY_WB);
  DEBUG ((DEBUG_ERROR, "CAMBOOT: memory 0x%lx+0x%lx %r\n", Base, Size, Status));
}

STATIC
VOID
PublishInitrd (
  IN CONST VOID  *Fdt
  )
{
  INT32        Chosen;
  CONST VOID  *StartProp;
  CONST VOID  *EndProp;
  INT32        StartLength;
  INT32        EndLength;
  UINT64       Start;
  UINT64       End;
  EFI_HANDLE   Handle;
  EFI_STATUS   Status;

  Chosen = FdtPathOffset (Fdt, "/chosen");
  if (Chosen < 0) {
    DEBUG ((DEBUG_ERROR, "CAMBOOT: no /chosen\n"));
    return;
  }

  StartProp = FdtGetProp (Fdt, Chosen, "linux,initrd-start", &StartLength);
  EndProp   = FdtGetProp (Fdt, Chosen, "linux,initrd-end", &EndLength);
  if ((StartProp == NULL) || (EndProp == NULL) ||
      ((StartLength != 4) && (StartLength != 8)) ||
      ((EndLength != 4) && (EndLength != 8))) {
    DEBUG ((DEBUG_ERROR, "CAMBOOT: no initrd properties\n"));
    return;
  }

  Start = (StartLength == 8) ?
    Fdt64ToCpu (ReadUnaligned64 (StartProp)) :
    Fdt32ToCpu (ReadUnaligned32 (StartProp));
  End = (EndLength == 8) ?
    Fdt64ToCpu (ReadUnaligned64 (EndProp)) :
    Fdt32ToCpu (ReadUnaligned32 (EndProp));
  if ((End <= Start) || (((End - Start) % RAM_BLOCK_SIZE) != 0)) {
    DEBUG ((DEBUG_ERROR, "CAMBOOT: invalid initrd 0x%lx..0x%lx\n", Start, End));
    return;
  }

  AddMemory (Start, End - Start);
  if ((End - Start) <= (UINT64)2048 * RAM_BLOCK_SIZE) {
    DEBUG ((DEBUG_ERROR, "CAMBOOT: initrd too small 0x%lx\n", End - Start));
    return;
  }

  /* evergo cold boot: the whole boot.img ramdisk is our FAT image (loader.efi + boot.xnup). */
  mRamBase       = (UINT8 *)(UINTN)Start;
  mRamSize       = End - Start;
  mMedia.MediaId = 1;
  mMedia.RemovableMedia   = FALSE;
  mMedia.MediaPresent     = TRUE;
  mMedia.LogicalPartition = TRUE;
  mMedia.ReadOnly         = TRUE;
  mMedia.WriteCaching     = FALSE;
  mMedia.BlockSize        = RAM_BLOCK_SIZE;
  mMedia.IoAlign          = 1;
  mMedia.LastBlock        = mRamSize / RAM_BLOCK_SIZE - 1;
  mDevicePath.Partition.PartitionSize = mRamSize / RAM_BLOCK_SIZE;

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiDevicePathProtocolGuid,
                  &mDevicePath,
                  &gEfiBlockIoProtocolGuid,
                  &mBlockIo,
                  &gEfiDiskIoProtocolGuid,
                  &mDiskIo,
                  NULL
                  );
  DEBUG ((
    DEBUG_ERROR,
    "CAMBOOT: initrd block 0x%lx size 0x%lx boot=%02x%02x%02x%02x %r\n",
    Start,
    End - Start,
    mRamBase[0],
    mRamBase[1],
    mRamBase[2],
    mRamBase[3],
    Status
    ));
}

STATIC
VOID
PublishFramebuffer (
  IN CONST VOID  *Fdt
  )
{
  INT32        Chosen;
  CONST VOID  *Video;
  INT32        VideoLength;
  UINT64       Base;
  UINT64       LogicalSize;
  FB_HANDOFF  *Handoff;
  FB_RUN_MAP  *Map;

  Chosen = FdtPathOffset (Fdt, "/chosen");
  if (Chosen < 0) {
    return;
  }
  Video = FdtGetProp (Fdt, Chosen, "atag,videolfb", &VideoLength);
  if ((Video == NULL) || (VideoLength < 20)) {
    DEBUG ((DEBUG_WARN, "CAMBOOT: no atag,videolfb\n"));
    return;
  }

  Base = Fdt64ToCpu (ReadUnaligned64 (Video));
  /* evergo LK puts the base in the 4th 32-bit cell of atag,videolfb
     (cell[0]=0x617c, cell[1]=0, cell[2]=vram, cell[3]=fb_base). */
  if ((Base == 0) || ((Base & 0xFFF) != 0) ||
      (Base < 0x40000000ULL) || (Base >= 0x240000000ULL)) {
    if (VideoLength >= 16) {
      Base = Fdt32ToCpu (ReadUnaligned32 ((CONST UINT8 *)Video + 12));
    }
  }
  if ((Base == 0) || ((Base & 0xFFF) != 0) ||
      (Base < 0x40000000ULL) || (Base >= 0x240000000ULL)) {
    DEBUG ((DEBUG_ERROR, "CAMBOOT: invalid framebuffer base 0x%lx\n", Base));
    return;
  }

  LogicalSize = (UINT64)FB_PITCH * FB_HEIGHT;
  Handoff = (FB_HANDOFF *)(UINTN)FB_HANDOFF_ADDR;
  //
  // The same page also carries the hot-jump payload handoff at +0x780.
  // Never clear the whole page here.
  //
  ZeroMem (Handoff, sizeof (*Handoff));
  Handoff->Magic  = FB_HANDOFF_MAGIC;
  Handoff->Base   = Base;
  Handoff->Width  = FB_WIDTH;
  Handoff->Height = FB_HEIGHT;
  Handoff->Pitch  = FB_PITCH;
  Handoff->Depth  = FB_DEPTH;

  Map = (FB_RUN_MAP *)(UINTN)FB_PAGE_MAP_ADDR;
  Map->Magic       = FB_PAGE_MAP_MAGIC;
  Map->LogicalSize = LogicalSize;
  Map->PageSize    = EFI_PAGE_SIZE;
  Map->RunCount    = 1;
  Map->Runs[0].Physical = Base;
  Map->Runs[0].Pages    = (UINT32)EFI_SIZE_TO_PAGES (LogicalSize);

  DEBUG ((DEBUG_ERROR, "CAMBOOT: framebuffer 0x%lx %ux%u pitch %u\n",
          Base, FB_WIDTH, FB_HEIGHT, FB_PITCH));
}


/**
  Accept a framebuffer handoff already published by the hot-jump producer.

  The Linux module writes FB_HANDOFF/FB_RUN_MAP with the LIVE scanout address
  taken from the DPU registers and translated through the IOMMU. That address is
  the only buffer the panel actually scans, so it must win over the compile-time
  constants. Overwriting it here is what kept every previous hot jump black.

  Returns TRUE when a usable handoff was found (and left untouched).
**/
STATIC
BOOLEAN
AdoptProducerHandoff (
  OUT UINT64  *Base
  )
{
  FB_HANDOFF   *Handoff = (FB_HANDOFF *)(UINTN)FB_HANDOFF_ADDR;
  FB_RUN_MAP   *Map     = (FB_RUN_MAP *)(UINTN)FB_PAGE_MAP_ADDR;

  if (Handoff->Magic != FB_HANDOFF_MAGIC) {
    return FALSE;
  }

  if ((Handoff->Base == 0) || ((Handoff->Base & 0xFFF) != 0) ||
      (Handoff->Width < 640) || (Handoff->Height < 475) ||
      (Handoff->Pitch < Handoff->Width * 4) || (Handoff->Pitch > 0x10000)) {
    return FALSE;
  }

  if ((Map->Magic != FB_PAGE_MAP_MAGIC) || (Map->RunCount == 0) ||
      (Map->Runs[0].Physical != Handoff->Base)) {
    return FALSE;
  }

  DEBUG ((DEBUG_ERROR, "CAMBOOT: adopting producer FB handoff 0x%lx %ux%u pitch %u\n",
          Handoff->Base, Handoff->Width, Handoff->Height, Handoff->Pitch));
  *Base = Handoff->Base;
  return TRUE;
}

//
// OVL0_2L register block (the layer Android's DRM commits to on this panel).
// Offsets per mtk_disp_ovl.c MT6833 definitions.
//
#define OVL2L_BASE            0x14006000UL
#define OVL_REG_DATAPATH_CON  0x024
#define OVL_REG_SRC_PITCH_MSB 0x040
#define OVL_REG_SRC_PITCH     0x044
#define OVL_REG_LX_ADDR       0x0F40
#define OVL_REG_LX_HDR_ADDR   0x0F44
#define OVL_REG_LX_HDR_PITCH  0x0F48
#define OVL_FBDC_L0_BIT       (1U << 4)

/**
  Switch the active OVL layer from FBDC/AFBC to linear addressing.

  The producer (Android) died before the jump, so its DRM commit thread can
  no longer reprogram the layer; register writes take effect immediately
  (mt6833 support_shadow=false). The AFBC body address already programmed in
  LX_ADDR is the linear frame start, so switching is:

    1. DATAPATH_CON: clear the layer FBDC bit
    2. SRC_PITCH:    plain byte pitch (was pitch_bytes * tile_h)
    3. SRC_PITCH_MSB: clear the pitch-high bits (was (pitch*8)>>16)

  LX_ADDR / LX_HDR_ADDR are left alone: the header region stays in the buffer
  but is no longer read once FBDC is off.

  Returns TRUE when the registers now read back as linear; FALSE on any
  mismatch (caller must then keep the FBDC flag set and not draw).
**/
STATIC
BOOLEAN
OvlSwitchLayerToLinear (
  IN  UINT32  BytePitch,
  OUT UINT32  *ReadbackDatapath,
  OUT UINT32  *ReadbackAddr
  )
{
  volatile UINT32  *R  = (volatile UINT32 *)(UINTN)OVL2L_BASE;
  UINT32            Dp;
  UINT32            PitchRaw;
  UINT32            PitchMsb;
  UINT32            Pitch;
  UINT32            NewMsb;
  UINT32            HdrAddr;
  UINT32            OldAddr;

  Dp        = R[OVL_REG_DATAPATH_CON / 4];
  PitchRaw  = R[OVL_REG_SRC_PITCH / 4];
  PitchMsb  = R[OVL_REG_SRC_PITCH_MSB / 4];

  OldAddr = R[OVL_REG_LX_ADDR / 4];
  if ((Dp & OVL_FBDC_L0_BIT) == 0) {
    /* Already linear; nothing to do. */
    *ReadbackDatapath = Dp;
    *ReadbackAddr     = OldAddr;
    return TRUE;
  }

  //
  // Use the pitch the PRODUCER already decoded (Handoff->Pitch, plain bytes).
  // Do not re-derive it from the registers: while FBDC is on, SRC_PITCH holds
  // pitch*tile_h, and this SoC leaves the pitch-MSB field at 0, so a heuristic
  // cannot tell the two encodings apart. Getting it wrong by the tile factor
  // makes the panel show the frame folded into 8x-repeated bands.
  //
  Pitch = BytePitch;
  if ((Pitch == 0) || (Pitch != (PitchRaw >> 3)) ) {
    DEBUG ((DEBUG_ERROR,
      "CAMBOOT: pitch check raw 0x%x -> byte 0x%x (msb 0x%x)\n",
      PitchRaw, Pitch, PitchMsb));
  }
  if (Pitch == 0) {
    return FALSE;
  }

  /* 1. clear the layer-0 FBDC enable */
  R[OVL_REG_DATAPATH_CON / 4] = Dp & ~OVL_FBDC_L0_BIT;

  /* 2. plain byte pitch */
  R[OVL_REG_SRC_PITCH / 4] = Pitch & 0xFFFF;

  /* 3. pitch MSB field cleared (linear uses no x8 encoding) */
  NewMsb = PitchMsb & ~0xF;
  R[OVL_REG_SRC_PITCH_MSB / 4] = NewMsb;

  //
  // 4. Point the layer at the BUFFER BASE, not at the AFBC body.
  //   0x0F40 (LX_ADDR) holds body = base + header_offset while FBDC is on;
  //   0x0F44 (LX_HDR_ADDR) holds the header, i.e. the real buffer base.
  //   Linear scanout now starts at the base and covers header+body, so the
  //   whole 10,444,800-byte frame stays inside one ION allocation instead of
  //   running 0x28000 past the end.
  //
  HdrAddr = R[OVL_REG_LX_HDR_ADDR / 4];
  if (HdrAddr != 0) {
    R[OVL_REG_LX_ADDR / 4] = HdrAddr;
  }
  *ReadbackAddr = R[OVL_REG_LX_ADDR / 4];

  asm volatile ("dsb sy" ::: "memory");

  /* Verify the writes landed. */
  Dp       = R[OVL_REG_DATAPATH_CON / 4];
  PitchRaw = R[OVL_REG_SRC_PITCH / 4];
  PitchMsb = R[OVL_REG_SRC_PITCH_MSB / 4];
  *ReadbackDatapath = Dp;

  DEBUG ((DEBUG_ERROR,
    "CAMBOOT: OVL->linear: dp 0x%x->0x%x pitch 0x%x msb 0x%x->0x%x\n",
    0, Dp, PitchRaw, 0, PitchMsb));

  return ((Dp & OVL_FBDC_L0_BIT) == 0) && (PitchRaw == (Pitch & 0xFFFF));
}

/**
  Publish the framebuffer handoff and run map from compile-time constants.
  Same structure SimpleFbDxe expects; used when no DTB is available.
**/
STATIC
VOID
PublishFbConstants (
  IN UINT64  Base
  )
{
  FB_HANDOFF  *Handoff = (FB_HANDOFF *)(UINTN)FB_HANDOFF_ADDR;
  FB_RUN_MAP  *Map     = (FB_RUN_MAP *)(UINTN)FB_PAGE_MAP_ADDR;
  UINT64       LogicalSize = (UINT64)FB_PITCH * FB_HEIGHT;

  ZeroMem (Handoff, sizeof (*Handoff));
  Handoff->Magic  = FB_HANDOFF_MAGIC;
  Handoff->Base   = Base;
  Handoff->Width  = FB_WIDTH;
  Handoff->Height = FB_HEIGHT;
  Handoff->Pitch  = FB_PITCH;
  Handoff->Depth  = FB_DEPTH;

  Map->Magic       = FB_PAGE_MAP_MAGIC;
  Map->LogicalSize = LogicalSize;
  Map->PageSize    = EFI_PAGE_SIZE;
  Map->RunCount    = 1;
  Map->Runs[0].Physical = Base;
  Map->Runs[0].Pages    = (UINT32)EFI_SIZE_TO_PAGES (LogicalSize);

  DEBUG ((DEBUG_ERROR, "CAMBOOT: framebuffer (constants) 0x%lx %ux%u pitch %u\n",
          Base, FB_WIDTH, FB_HEIGHT, FB_PITCH));
}

/**
  Translate a logical byte offset through the producer's run map. The scanout
  is contiguous in IOVA space but may be scattered in physical memory, so a
  raw Base + offset write would land on the wrong page (or unmapped memory)
  for every run after the first break.
  Returns NULL when the map is invalid or the offset is beyond LogicalSize.
**/
STATIC
UINT8 *
RunMapTranslate (
  IN FB_RUN_MAP  *Map,
  IN UINT64       Offset
  )
{
  UINT64  PageIndex;
  UINT64  RunEnd;
  UINT32  PageOffset;
  UINT32  Index;

  if ((Map == NULL) || (Map->Magic != FB_PAGE_MAP_MAGIC) ||
      (Map->PageSize == 0) || (Map->RunCount == 0) ||
      (Map->RunCount > 100U) || (Offset >= Map->LogicalSize)) {
    return NULL;
  }

  PageIndex  = Offset / Map->PageSize;
  PageOffset = (UINT32)(Offset % Map->PageSize);
  RunEnd     = 0;
  for (Index = 0; Index < Map->RunCount; Index++) {
    RunEnd += Map->Runs[Index].Pages;
    if (PageIndex < RunEnd) {
      UINT64  RunStart = RunEnd - Map->Runs[Index].Pages;
      return (UINT8 *)(UINTN)(
        Map->Runs[Index].Physical +
        (PageIndex - RunStart) * Map->PageSize +
        PageOffset
        );
    }
  }
  return NULL;
}

/**
  Prepare the adopted producer scanout for linear drawing: switch the OVL
  layer out of FBDC/AFBC when the producer flagged it, and clear the flag in
  the handoff on success so DrawTestPattern's fail-closed gate opens.
  Returns TRUE when drawing may proceed.
**/
STATIC
BOOLEAN
PrepareLinearScanout (
  VOID
  )
{
  volatile UINT32  *Flags = (volatile UINT32 *)(
    (UINT8 *)(UINTN)FB_PAGE_MAP_ADDR + FB_MAP_FLAGS_OFFSET);
  FB_HANDOFF       *Handoff = (FB_HANDOFF *)(UINTN)FB_HANDOFF_ADDR;
  UINT32            Dp = 0;
  UINT32            Addr = 0;

  if ((*Flags & FB_MAP_FLAG_FBDC) == 0) {
    return TRUE;   /* already linear */
  }

  if (!OvlSwitchLayerToLinear (Handoff->Pitch, &Dp, &Addr)) {
    TRC (0x0702, TRC_ERROR, 5, Dp, Addr, 0, 0);
    DEBUG ((DEBUG_ERROR, "CAMBOOT: OVL linear switch FAILED (dp 0x%x addr 0x%x)\n", Dp, Addr));
    return FALSE;
  }

  *Flags &= ~(UINT32)FB_MAP_FLAG_FBDC;
  asm volatile ("dsb sy" ::: "memory");
  // Record the post-switch register state: dp (bit4 must be 0) and the new
  // LX_ADDR (buffer base). These two values are the evidence that the layer
  // really is linear and starts at the base.
  TRC (0x0702, TRC_ERROR, 6, Dp, Addr, 0, 0);
  DEBUG ((DEBUG_ERROR, "CAMBOOT: OVL->linear done dp 0x%x LX_ADDR 0x%x, FBDC flag cleared\n",
          Dp, Addr));
  return TRUE;
}

/**
  Fill the panel with six vertical bars THROUGH the run map and read one pixel
  back out of each THROUGH the map as well. Returns a bitmask of the bars whose
  readback matched what was written, so the caller can tell "the writes reached
  DRAM" (bit set) from "nothing happened".
  Falls back to direct Base+offset addressing only when the producer map is
  invalid or does not cover the pattern (single-run producers).
**/
STATIC
UINT32
DrawTestPattern (
  IN UINT64  Base
  )
{
  STATIC CONST UINT32  Bars[6] = {
    0x00FF0000,   /* B,G,R,A little endian: blue  */
    0x0000FF00,   /* green */
    0x000000FF,   /* red   */
    0x00FFFFFF,   /* white */
    0x0000FFFF,   /* yellow */
    0x00000000    /* black */
  };
  FB_RUN_MAP       *Map = (FB_RUN_MAP *)(UINTN)FB_PAGE_MAP_ADDR;
  UINT32            Flags = *(volatile UINT32 *)(
                            (UINT8 *)(UINTN)FB_PAGE_MAP_ADDR + FB_MAP_FLAGS_OFFSET);
  BOOLEAN            UseMap;
  UINT32            Mask = 0;
  UINT32            BarWidth = FB_WIDTH / 6;
  UINT32            X, Y;
  UINT64            LineBytes = (UINT64)FB_PITCH;

  //
  // FBDC/AFBC layers carry a header before the body: a linear write would be
  // decoded as compressed data and show garbage (the scrambled-fragments
  // symptom). Fail closed - draw nothing and record why - until the caller
  // switches the layer to linear.
  //
  if ((Flags & FB_MAP_FLAG_FBDC) != 0) {
    TRC (0x0702, TRC_ERROR, 2, Base, Flags, 0, 0);
    DEBUG ((DEBUG_ERROR,
      "CAMBOOT: scanout layer is FBDC/AFBC (flags 0x%x); refusing linear draw\n",
      Flags));
    *(volatile UINT64 *)(UINTN)(0x4811FC00ULL + 14 * 8) =
      0xFB00000000000000ULL | (0x2ULL << 16);
    *(volatile UINT64 *)(UINTN)(0x4811FC00ULL + 15 * 8) = Base;
    asm volatile ("dsb sy" ::: "memory");
    return 0;
  }

  //
  // Use the map only when it is valid AND provably covers the whole pattern
  // (the last pixel must translate). This guards against a map published for
  // a smaller/staler buffer than the pattern constants assume.
  //
  UseMap = (Map->Magic == FB_PAGE_MAP_MAGIC) && (Map->PageSize != 0) &&
           (Map->RunCount != 0) && (Map->RunCount <= 100U) &&
           (Map->LogicalSize >= (UINT64)FB_PITCH * FB_HEIGHT) &&
           (RunMapTranslate (Map, (UINT64)(FB_HEIGHT - 1) * LineBytes +
                              (UINT64)(FB_WIDTH - 1) * 4) != NULL);

  for (Y = 0; Y < FB_HEIGHT; Y++) {
    for (X = 0; X < FB_WIDTH; X++) {
      UINT32  Index = X / BarWidth;
      UINT32  Value;
      if (Index > 5) {
        Index = 5;
      }
      Value = Bars[Index];
      if (!UseMap) {
        /* Fail closed: no valid covering map means the buffer layout is not
         * proven, and a raw Base+X write could land on the wrong page or in
         * unmapped memory. Draw nothing rather than guess. */
        TRC (0x0702, TRC_ERROR, 3, Base, 0, 0, 0);
        DEBUG ((DEBUG_ERROR,
          "CAMBOOT: no valid covering FB run map; refusing direct draw\n"));
        *(volatile UINT64 *)(UINTN)(0x4811FC00ULL + 14 * 8) =
          0xFB00000000000000ULL | (0x3ULL << 16);
        *(volatile UINT64 *)(UINTN)(0x4811FC00ULL + 15 * 8) = Base;
        asm volatile ("dsb sy" ::: "memory");
        return 0;
      }
      {
        volatile UINT32  *P = (volatile UINT32 *)RunMapTranslate (
                              Map,
                              (UINT64)Y * LineBytes + (UINT64)X * 4
                              );
        if (P == NULL) {
          /* Mid-pattern translation failure: stop drawing, record where. */
          TRC (0x0702, TRC_ERROR, 4, Base, ((UINT64)Y << 16) | X, 0, 0);
          DEBUG ((DEBUG_ERROR,
            "CAMBOOT: run map translate failed at %ux%u; aborting pattern\n",
            X, Y));
          *(volatile UINT64 *)(UINTN)(0x4811FC00ULL + 14 * 8) =
            0xFB00000000000000ULL | (0x4ULL << 16) | ((UINT64)Y << 8) | (X & 0xFF);
          *(volatile UINT64 *)(UINTN)(0x4811FC00ULL + 15 * 8) = Base;
          asm volatile ("dsb sy" ::: "memory");
          return 0;
        }
        *P = Value;
      }
    }
  }

  asm volatile ("dsb sy" ::: "memory");

  for (UINT32 I = 0; I < 6; I++) {
    UINT32  Read;
    UINT64  Off = (UINT64)(FB_HEIGHT / 2) * LineBytes +
                  (UINT64)(I * BarWidth + BarWidth / 2) * 4;
    volatile UINT32  *P = (volatile UINT32 *)RunMapTranslate (Map, Off);
    if (P == NULL) {
      continue;
    }
    Read = *P;
    if (Read == Bars[I]) {
      Mask |= (1U << I);
    }
  }

  /* Leave the readback evidence where the module can dump it. Bit 8 of the
   * witness records the map path (always 1 now: direct addressing is gone). */
  *(volatile UINT64 *)(UINTN)(0x4811FC00ULL + 14 * 8) =
    0xFB00000000000000ULL | ((UINT64)UseMap << 8) | Mask;
  *(volatile UINT64 *)(UINTN)(0x4811FC00ULL + 15 * 8) = Base;
  asm volatile ("dsb sy" ::: "memory");
  return Mask;
}

EFI_STATUS
EFIAPI
CamelliaBootMemoryEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINT64  Dtb;

  Dtb = *(volatile UINT64 *)(UINTN)DTB_POINTER_ADDR;
  if ((Dtb == 0) || ((Dtb & 3) != 0)) {
    DEBUG ((DEBUG_WARN, "CAMBOOT: no bootloader DTB pointer (0x%lx)\n", Dtb));
    //
    // Hot-jump: the module never had a DTB to hand over, but it DOES publish the
    // live DPU scanout through FB_HANDOFF. Prefer that over the constants, and
    // only fall back to the compile-time values when no producer handoff exists.
    //
    {
      UINT64 ProducerBase = 0;

      if (AdoptProducerHandoff (&ProducerBase) && PrepareLinearScanout ()) {
        UINT32 MaskLive = DrawTestPattern (ProducerBase);

        *(volatile UINT64 *)(UINTN)(0x4811FC00ULL + 14 * 8) =
          0xFB00000000000000ULL | ((UINT64)MaskLive << 8) | MaskLive;
        *(volatile UINT64 *)(UINTN)(0x4811FC00ULL + 15 * 8) = ProducerBase;
        asm volatile ("dsb sy" ::: "memory");
        TRC (0x0702, TRC_EXIT, 1, ProducerBase, MaskLive, 0, Dtb);
        DEBUG ((DEBUG_ERROR, "CAMBOOT: live-FB pattern mask 0x%x at 0x%lx\n",
                MaskLive, ProducerBase));
        //
        // Short hold only: the pattern has already been proven on the panel,
        // and the watchdog window (31 s) is a hard budget for the whole boot,
        // so every second spent holding is a second the loader does not get.
        //
        TRC (0x0702, TRC_EXIT, 7, ProducerBase, 2, 0, 0);
        gBS->Stall (2 * 1000 * 1000);
        TRC (0x0702, TRC_EXIT, 8, ProducerBase, 0, 0, 0);
        return EFI_SUCCESS;
      }
    }

    PublishFbConstants (FB_BASE_LK);
    {
      UINT32 MaskLk  = DrawTestPattern (FB_BASE_LK);
      UINT32 MaskAnd = DrawTestPattern (FB_BASE_EVERGO);

      /* Keep both readback masks: bit i set = bar i read back unchanged. */
      *(volatile UINT64 *)(UINTN)(0x4811FC00ULL + 14 * 8) = 0xFB00000000000000ULL |
                                                            ((UINT64)MaskAnd << 8) | MaskLk;
      *(volatile UINT64 *)(UINTN)(0x4811FC00ULL + 15 * 8) = FB_BASE_LK;
      asm volatile ("dsb sy" ::: "memory");
      TRC (0x0701, TRC_EXIT, 0, FB_BASE_LK, MaskLk, MaskAnd, Dtb);
      DEBUG ((DEBUG_ERROR, "CAMBOOT: pattern masks LK 0x%x Android 0x%x\n", MaskLk, MaskAnd));
    }
    return EFI_SUCCESS;
  }

  if (FdtCheckHeader ((VOID *)(UINTN)Dtb) != 0) {
    //
    // MTK does not necessarily point at the FDT itself: the boot image's DTB
    // area starts with a 64-byte MTK header (size 0x40, then the FDT), so the
    // handoff address can be 0x40 before the flattened tree. Probe forward a
    // bounded window instead of giving up on the first mismatch; the DTB was
    // just loaded right here, so this stays inside valid memory.
    //
    UINT64  Probe;
    UINT64  Found = 0;

    for (Probe = Dtb + 8; Probe < Dtb + 0x1000; Probe += 8) {
      INT32  Total;

      if (FdtCheckHeader ((VOID *)(UINTN)Probe) != 0) {
        continue;
      }

      Total = FdtTotalSize ((VOID *)(UINTN)Probe);
      if ((Total >= 0x100) && (Total <= 0x100000)) {
        Found = Probe;
        break;
      }
    }

    if (Found == 0) {
      DEBUG ((DEBUG_WARN, "CAMBOOT: no valid FDT at 0x%lx or within +0x1000\n", Dtb));
      /* Same hot-jump fallback as the "no pointer" case: without it the stale
         slot value silently suppresses both the GOP handoff and the pattern. */
      {
        UINT64 ProducerBase = 0;

        if (AdoptProducerHandoff (&ProducerBase) && PrepareLinearScanout ()) {
          UINT32 MaskLive = DrawTestPattern (ProducerBase);

          *(volatile UINT64 *)(UINTN)(0x4811FC00ULL + 14 * 8) =
            0xFB00000000000000ULL | ((UINT64)MaskLive << 8) | MaskLive;
          *(volatile UINT64 *)(UINTN)(0x4811FC00ULL + 15 * 8) = ProducerBase;
          asm volatile ("dsb sy" ::: "memory");
          TRC (0x0702, TRC_EXIT, 1, ProducerBase, MaskLive, 0, Dtb);
          DEBUG ((DEBUG_ERROR, "CAMBOOT: live-FB pattern mask 0x%x at 0x%lx\n",
                  MaskLive, ProducerBase));
          return EFI_SUCCESS;
        }
      }

      PublishFbConstants (FB_BASE_LK);
      {
        UINT32 MaskLk  = DrawTestPattern (FB_BASE_LK);
        UINT32 MaskAnd = DrawTestPattern (FB_BASE_EVERGO);

        *(volatile UINT64 *)(UINTN)(0x4811FC00ULL + 14 * 8) = 0xFB00000000000000ULL |
                                                              ((UINT64)MaskAnd << 8) | MaskLk;
        *(volatile UINT64 *)(UINTN)(0x4811FC00ULL + 15 * 8) = FB_BASE_LK;
        asm volatile ("dsb sy" ::: "memory");
        TRC (0x0701, TRC_EXIT, 0, FB_BASE_LK, MaskLk, MaskAnd, Dtb);
      DEBUG ((DEBUG_ERROR, "CAMBOOT: pattern masks LK 0x%x Android 0x%x\n", MaskLk, MaskAnd));
      }
      return EFI_SUCCESS;
    }

    DEBUG ((DEBUG_INFO, "CAMBOOT: FDT found at 0x%lx (+0x%lx from handoff)\n",
            Found, Found - Dtb));
    Dtb = Found;
  }

  DEBUG ((DEBUG_ERROR, "CAMBOOT: DTB 0x%lx size 0x%x\n",
          Dtb, FdtTotalSize ((VOID *)(UINTN)Dtb)));
  PublishFramebuffer ((VOID *)(UINTN)Dtb);
  PublishInitrd ((VOID *)(UINTN)Dtb);
  return EFI_SUCCESS;
}
