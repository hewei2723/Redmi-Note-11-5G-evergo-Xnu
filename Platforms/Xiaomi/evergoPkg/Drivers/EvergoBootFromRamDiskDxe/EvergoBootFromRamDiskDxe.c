/** @file
  evergo cold-boot: start \EFI\BOOT\BOOTAA64.EFI from the boot.img RAM disk.

  CamelliaBootMemoryDxe publishes the LK-provided initrd (our FAT image) as a
  BlockIo/DiskIo device. FatPkg turns it into SimpleFileSystem. The Silicium
  BootManagerLib only registers FV boot options, so nothing would boot from that
  volume. This driver finds BOOTAA64.EFI on any SimpleFileSystem volume and
  starts it (loader-evergo.efi), which then reads \EFI\HackiOS\boot.xnup and
  hands off to XNU. If no such file exists it returns and BDS continues.
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/LoadedImage.h>
#include <Guid/FileInfo.h>

STATIC CONST CHAR16  mLoaderPath[] = L"\\EFI\\BOOT\\BOOTAA64.EFI";

STATIC
EFI_STATUS
TryVolume (
  IN EFI_HANDLE  ImageHandle,
  IN EFI_HANDLE  FsHandle
  )
{
  EFI_STATUS                       Status;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
  EFI_FILE_PROTOCOL               *Root = NULL;
  EFI_FILE_PROTOCOL               *File = NULL;
  EFI_FILE_INFO                   *Info = NULL;
  UINTN                            InfoSize = 0;
  VOID                            *Buffer = NULL;
  UINTN                            Size;
  EFI_HANDLE                       Loaded = NULL;
  UINTN                            ExitDataSize;
  CHAR16                          *ExitData = NULL;

  Status = gBS->HandleProtocol (FsHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = Fs->OpenVolume (Fs, &Root);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = Root->Open (Root, &File, (CHAR16 *)mLoaderPath, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    Root->Close (Root);
    return Status;
  }

  // Read EFI_FILE_INFO to get the file size.
  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    goto out;
  }
  Info = AllocatePool (InfoSize);
  if (Info == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto out;
  }
  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
  if (EFI_ERROR (Status)) {
    goto out;
  }
  Size = (UINTN)Info->FileSize;
  Buffer = AllocatePool (Size);
  if (Buffer == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto out;
  }
  Status = File->Read (File, &Size, Buffer);
  if (EFI_ERROR (Status) || Size == 0) {
    goto out;
  }

  DEBUG ((DEBUG_ERROR, "EVERGOBOOT: loading %s (%u bytes) from RAM disk\n", mLoaderPath, (UINT32)Size));
  Status = gBS->LoadImage (TRUE, ImageHandle, NULL, Buffer, Size, &Loaded);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "EVERGOBOOT: LoadImage failed: %r\n", Status));
    goto out;
  }
  DEBUG ((DEBUG_ERROR, "EVERGOBOOT: StartImage\n"));
  Status = gBS->StartImage (Loaded, &ExitDataSize, &ExitData);
  DEBUG ((DEBUG_ERROR, "EVERGOBOOT: loader returned: %r\n", Status));

out:
  if (Buffer != NULL) {
    FreePool (Buffer);
  }
  if (Info != NULL) {
    FreePool (Info);
  }
  if (File != NULL) {
    File->Close (File);
  }
  if (Root != NULL) {
    Root->Close (Root);
  }
  return Status;
}

EFI_STATUS
EFIAPI
EvergoBootFromRamDiskEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE *Handles = NULL;
  UINTN       Count = 0;
  UINTN       i;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );
  if (EFI_ERROR (Status) || (Handles == NULL)) {
    return EFI_SUCCESS;
  }

  for (i = 0; i < Count; i++) {
    Status = TryVolume (ImageHandle, Handles[i]);
    if (!EFI_ERROR (Status)) {
      // If StartImage returned success the loader completed; try next volume.
      // If it never returns, we are in XNU.
    }
  }

  FreePool (Handles);

  // Nothing booted: let BDS continue.
  return EFI_SUCCESS;
}
