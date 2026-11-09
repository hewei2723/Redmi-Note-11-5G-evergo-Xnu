#ifndef _XNU_LOADER_H_
#define _XNU_LOADER_H_

//
// XNUP payload container and in-firmware loader.
//
// Origin: hackios-pencilpen1/uefi/files/.../BootManagerLib (device: redwood),
// ported for evergo. On redwood the boot shim copies everything appended after
// the UEFI FD to XNU_PAYLOAD_BASE; that address is reserved for the payload.
//
// evergo has no BootShim on the hot-jump path, so the container address is the
// EXISTING no-map DTB reservation "xnup-payload@B5400000"
// (reg = 0xb5400000 + 0x4000000). The DXE heap occupies that node's 16 MiB
// tail (0xb6c00000), so the payload window is the 24 MiB before it.
//
#define XNU_PAYLOAD_BASE      0xB5400000ULL
#define XNU_PAYLOAD_MAX_SIZE  0x01800000ULL   /* 0xb5400000..0xb6c00000 (24 MiB) */
#define XNU_PAYLOAD_MAGIC     0x50554E58      /* "XNUP" */

typedef struct {
  UINT32  Magic;
  UINT32  Size;
} XNU_PAYLOAD_HEADER;

// The header is followed by entries; an empty name ends the list.
typedef struct {
  CHAR8   Name[16];
  UINT32  Size;
  UINT32  Reserved;
  // UINT8 Data[Size], padded to 16 bytes
} XNU_PAYLOAD_ENTRY;

// Raw GPT partition that receives the text log (read back from Android).
// evergo does not necessarily have this partition; FlushLog() tolerates that.
#define XNU_LOG_PARTITION  L"logdump"

#define XNU_MARKER_MAGIC  0x584E5521 // "XNU!"

//
// TRUE when a plausible XNUP container sits at XNU_PAYLOAD_BASE.
//
BOOLEAN
EFIAPI
XnuLoaderPayloadPresent (
  VOID
  );

//
// Start "loader.efi" from the container when present; otherwise write the
// stage-0 diagnostic log. Returns to the caller when there is nothing to do.
//
VOID
EFIAPI
XnuLoaderMain (
  VOID
  );

#endif /* _XNU_LOADER_H_ */
