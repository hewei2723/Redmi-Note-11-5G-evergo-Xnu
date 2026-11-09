_Head:
  adr   x1, _Payload    /* Set _Entry address */
  b     _Start          /* Jump to the real code */

#if REQUIRES_KERNEL_HEADER == 1
/* Linux arm64 Image header per Documentation/arch/arm64/booting.rst:
     0x00 code (2 instrs: adr x1,_Payload; b _Start - the Image entry),
     0x08 text_offset  0x10 image_size  0x18 flags
     0x20/0x28/0x30 reserved, 0x38 magic "ARM\x64", 0x3C res4.
   image_size is patched at pack time (build_xnu_image.sh) because the
   assembler cannot know the FD + optional payload length. */
_StackSize:
  .quad 0x00080000      /* 0x08 Text Offset: standard Image load offset */
  .quad 0               /* 0x10 Image Size - patched by build script */
  .quad 0               /* 0x18 Flags */
  .quad 0               /* 0x20 Reserved */
  .quad 0               /* 0x28 Reserved */
  .quad 0               /* 0x30 Reserved */
  .ascii "ARM\x64"      /* 0x38 Magic */
  .long 0               /* 0x3C Reserved */
#endif

.balign 8
_Start:
  /* witness #0b: shim entry reached (0xB6430F10 = 0x5A5A5A5A5A5A0000). The
     original "first instruction" witness was removed because the ARM64
     Image header must have its magic at 0x38, leaving no room before it.
     _Start is the Image entry (load + 0x40), so this marker proves LK
     executed at least the standard Image entry point. */
  movz  x7, #0x0F10
  movk  x7, #0xB643, lsl #16
  movz  x8, #0x0000
  movk  x8, #0x5A5A, lsl #16
  movk  x8, #0x5A5A, lsl #32
  str   x8, [x7]
  dsb   sy
  /* witness #1: shim entered. Stages land in the 0xB6430F00..0xB6430F20
     window right before the 64 KiB "Trace Copy B" end; Linux never touches
     this no-map reservation. Value = stage id + 0x5A5A0000_00000000 base. */
  movz  x7, #0x0F00
  movk  x7, #0xB643, lsl #16
  movz  x8, #0x5A5A
  movk  x8, #0x5A5A, lsl #16
  str   x8, [x7]
  dsb   sy
  mov   x4, x1
  ldr   x5, =FD_BASE
  cmp   x4, x5
  beq   _Entry
  ldr   x6,  =FD_SIZE

_CopyLoop:
  ldp   x2, x3, [x4], #16
  stp   x2, x3, [x5], #16
  subs  x6, x6, #16
  b.ne  _CopyLoop
  ldr   x5, =FD_BASE
  /* witness #2: FD copy finished (value ..5A5A0002) */
  movz  x8, #0x0002
  movk  x8, #0x5A5A, lsl #16
  movk  x8, #0x5A5A, lsl #32
  str   x8, [x7]
  dsb   sy

_Entry:
#if defined(PAYLOAD_BASE) && defined(PAYLOAD_SIZE)
  /* Copy the "XNUP" container that follows the FD image in this boot image
     into the reserved payload window (see XnuLoader.h in BootManagerLib).
     x1 = _Payload (blob base), so the payload starts at _Payload + FD_SIZE. */
  ldr   x9, =FD_SIZE
  add   x4, x1, x9
  ldr   x5, =PAYLOAD_BASE
  ldr   x6, =PAYLOAD_SIZE
_PayloadCopy:
  ldp   x2, x3, [x4], #16
  stp   x2, x3, [x5], #16
  subs  x6, x6, #16
  b.ne  _PayloadCopy
  ldr   x5, =FD_BASE
#endif
  /* cold-boot handoff: save LK's DTB (x0) where CamelliaBootMemoryEntry reads it */
  movz  x7, #0x07F8
  movk  x7, #0xA400, lsl #16
  str   x0, [x7]
  /* witness: prove the shim reached the jump. 0xB6430800 sits right after
     trace copy B (TRC_BASE_B + 0x800) inside the Linux no-map XNUP
     reservation, so nothing on the Android side can overwrite it. The value
     is only written here, so magic present == shim jumped to the FD entry. */
  movz  x7, #0x0800
  movk  x7, #0xB643, lsl #16
  movz  x8, #0x4D45              /* 'EM' little-endian */
  movk  x8, #0x5348, lsl #16     /* 'HS' -> 0x53484D45 = bytes 'EMHS' */
  movk  x8, #0x4E45, lsl #32     /* 'EN'    */
  movk  x8, #0x0000, lsl #48
  str   x8, [x7]
  dsb   sy
  /* witness #3: final jump target staged (0xB6430F08 = FD entry base) */
  movz  x7, #0x0F08
  movk  x7, #0xB643, lsl #16
  movz  x8, #0x0003
  movk  x8, #0x5A5A, lsl #16
  movk  x8, #0x5A5A, lsl #32
  str   x8, [x7]
  dsb   sy
  br    x5

_Dead:
  b     _Dead

.text
/* Flush the literal pool BEFORE _Payload: any ldr =literal must live before
   the label, otherwise _Payload would point at a literal instead of the FD. */
.ltorg
.balign 16

_Payload:
