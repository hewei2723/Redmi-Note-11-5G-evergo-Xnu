/**
  Copyright (C) DuoWoA authors. All rights reserved.
  Copyright (C) Microsoft Corporation. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/PcdLib.h>
#include <Protocol/FirmwareVolume2.h>
#include <Library/DebugLib.h>
#include <Library/AcpiTableUpdateLib.h>
#include <Library/DeviceBootManagerLib.h>
#include <Library/PlatformBootManagerLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/SecureBootHandlerLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/MsBootOptionsLib.h>
#include <Library/BootGraphicsLib.h>
#include <Library/BootGraphics.h>
#include <Library/PrintLib.h>

#include <Configuration/BootDevices.h>

#include "BootManager.h"
#include "XnuLoader.h"

//
// Global Variables
//
STATIC UINTN XPos = 0;
STATIC UINTN YPos = 0;

//
// BDS Combo Message
//
STATIC CONST CHAR8 *ComboMessage = "[Volume Up] Boot Manager";

VOID
EFIAPI
PlatformBootManagerBeforeConsole ()
{
  // Signal End Of DXE
  EfiEventGroupSignal (&gEfiEndOfDxeEventGroupGuid);

  // Dispatch Deferred Images
  EfiBootManagerDispatchDeferredImages ();

  // Register Boot Options
  MsBootOptionsLibRegisterDefaultBootOptions ();

  // Register Console Variables
  EfiBootManagerUpdateConsoleVariable (ConIn,  (EFI_DEVICE_PATH_PROTOCOL *)&KeypadDevicePath,  NULL);
  EfiBootManagerUpdateConsoleVariable (ConOut, (EFI_DEVICE_PATH_PROTOCOL *)&DisplayDevicePath, NULL);
  EfiBootManagerUpdateConsoleVariable (ErrOut, (EFI_DEVICE_PATH_PROTOCOL *)&DisplayDevicePath, NULL);

  // Execute Secondary Before Console
  DeviceBootManagerBeforeConsole (NULL, NULL);
}

VOID
PostGopSetup (OUT EFI_GRAPHICS_OUTPUT_PROTOCOL **GopProtocol)
{
  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *LocalGopProtocol;

  // Verify Parameters
  if (GopProtocol == NULL) {
    return;
  }

  // Locate GOP Protocol
  Status = gBS->HandleProtocol (gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid, (VOID *)&LocalGopProtocol);
  if (EFI_ERROR (Status) && Status != EFI_UNSUPPORTED) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Locate GOP Protocol from Handle! Status = %r\n", __FUNCTION__, Status));
    return;
  }

  // Check Status
  if (Status == EFI_UNSUPPORTED) {
    // Locate GOP Protocol
    Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID *)&LocalGopProtocol);
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "%a: Failed to Locate GOP Protocol! Status = %r\n", __FUNCTION__, Status));
      return;
    }

    // Install GOP Protocol
    Status = gBS->InstallProtocolInterface (&gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid, EFI_NATIVE_INTERFACE, LocalGopProtocol);
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "%a: Failed to Install GOP Protocol onto \"ConsoleOutHandle\"! Status = %r\n", __FUNCTION__, Status));
    }
  }

  // Pass GOP Protocol
  *GopProtocol = LocalGopProtocol;
}

VOID
EFIAPI
PlatformBootManagerAfterConsole ()
{
  EFI_STATUS                    Status      = EFI_SUCCESS;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *GopProtocol = NULL;

  // Post Setup GOP
  PostGopSetup (&GopProtocol);

  // Display Boot Logo
  Status = DisplayBootGraphic (BG_SYSTEM_LOGO);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Display Boot Logo! Status = %r\n", __FUNCTION__, Status));
  }

  // Register Key Callback
  Status = RegisterKeyCallback ((EFI_DEVICE_PATH_PROTOCOL *)&KeypadDevicePath);
  if (!EFI_ERROR (Status) && GopProtocol != NULL) {
    // Set Screen Resolution
    UINT32 ScreenWidth  = GopProtocol->Mode->Info->HorizontalResolution;
    UINT32 ScreenHeight = GopProtocol->Mode->Info->VerticalResolution;

    // Set Combo Message Position
    XPos = (ScreenWidth - AsciiStrLen (ComboMessage) * EFI_GLYPH_WIDTH) / 2;
    YPos = ScreenHeight * 48 / 50;
  }

  // Execute Secondary After Console
  DeviceBootManagerAfterConsole ();

#ifdef EVERGO_SEC_BEACONS
  //
  // evergo hot-jump: if a valid "XNUP" container was placed in the reserved
  // payload window (XnuLoader.h), hand over to the XNU loader now. Inert when
  // nothing was placed, so the normal BDS path is unchanged. Bit 63 of beacon
  // slot 17 records durably that the loader ran, because the 0xAE/0xAF steps
  // are overwritten by the following BDS beacons.
  //
  //
  // Always enter the loader hook: it reports "no container" with its own
  // beacon (0xAD) instead of staying silent, which is what made a missing
  // payload indistinguishable from a crash inside BDS.
  //
  //
  // evergo 2026-11-09: try the UniXNU boot manager FIRST. If the UniXNU app
  // is present in the firmware volume it runs as a boot manager (menu) and
  // only returns when the user exits; the legacy XNU payload hook runs after
  // UniXNU declines.
  //
  {
    EFI_STATUS                       UStatus;
    EFI_HANDLE                      *FvHandles;
    UINTN                            FvCount, FvIndex;
    STATIC EFI_GUID  mUniXNUFileGuid = {
      0x9B665204, 0xA133, 0x47A2, { 0x93, 0x96, 0x9E, 0x2B, 0x37, 0x0A, 0x4E, 0x25 }
    };
    STATIC EFI_GUID  mFv2Guid = EFI_FIRMWARE_VOLUME2_PROTOCOL_GUID;

    if (!EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &mFv2Guid, NULL, &FvCount, &FvHandles))) {
      for (FvIndex = 0; FvIndex < FvCount; FvIndex++) {
        EFI_FIRMWARE_VOLUME2_PROTOCOL  *Fv;
        EFI_FV_FILETYPE                FileType;
        EFI_FV_FILE_ATTRIBUTES         Attrib;
        UINTN                          Size;
        VOID                          *Buffer;

        if (EFI_ERROR (gBS->HandleProtocol (FvHandles[FvIndex], &mFv2Guid, (VOID **)&Fv))) {
          continue;
        }
        Buffer = NULL;
        Size   = 0;
        if (EFI_ERROR (Fv->ReadFile (
                          Fv, &mUniXNUFileGuid, NULL, &Size,
                          &FileType, &Attrib, NULL)))
        {
          continue;
        }
        Buffer = AllocatePool (Size);
        if ((Buffer == NULL) ||
            EFI_ERROR (Fv->ReadFile (
                          Fv, &mUniXNUFileGuid, Buffer, &Size,
                          &FileType, &Attrib, NULL)))
        {
          if (Buffer != NULL) FreePool (Buffer);
          continue;
        }
        {
          EFI_HANDLE  Ui;
          if (!EFI_ERROR (gBS->LoadImage (
                             FALSE, gImageHandle, NULL, Buffer, Size, &Ui)))
          {
            UStatus = gBS->StartImage (Ui, NULL, NULL);
            DEBUG ((DEBUG_ERROR, "UniXNU menu returned %r\n", UStatus));
          }
        }
        FreePool (Buffer);
        break;
      }
      FreePool (FvHandles);
    }
  }

  XnuLoaderMain ();
  ((volatile UINT64 *)(UINTN)0x4811FC00ULL)[17] |= (1ULL << 63);
#endif
}

VOID
EFIAPI
PlatformBootManagerOnDemandConInConnect ()
{
  // Execute Secondary On Demand
  DeviceBootManagerOnDemandConInConnect ();
}

VOID
EFIAPI
PlatformBootManagerProcessBootCompletion (OUT EFI_BOOT_MANAGER_LOAD_OPTION *BootOption)
{
  return;
}

VOID
EFIAPI
PlatformBootManagerPriorityBoot (IN OUT UINT16 **BootNext)
{
  EFI_STATUS                   Status;
  EFI_BOOT_MANAGER_LOAD_OPTION LoadOption;
  BOOLEAN                      BootManagerFlag;

  // Unregister Key Callback
  UnregisterKeyCallback ();

  // Get Boot Manager Flag
  BootManagerFlag = EnterBootManager ();
  if (BootManagerFlag == FALSE) {
    return;
  }

  // Get Boot Manager Load Option
  Status = MsBootOptionsLibGetBootManagerMenu (&LoadOption, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to get Boot Manager Load Option! Status = %r", __FUNCTION__, Status));
    return;
  }

  // Execute Boot Manager Load Option
  EfiBootManagerBoot           (&LoadOption);
  EfiBootManagerFreeLoadOption (&LoadOption);
}

VOID
EFIAPI
PlatformBootManagerWaitCallback (IN UINT16 TimeoutRemain)
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Color;

  // Verify Combo Message Position
  if (XPos == 0 && YPos == 0) {
    return;
  }

  // Get Timeout Time
  UINT16 Timeout = PcdGet16 (PcdPlatformBootTimeOut);

  // Check Remaining Timeout
  if (TimeoutRemain == Timeout || TimeoutRemain == 0) {
    // Set Draw Color
    UINT8 DrawColor = (TimeoutRemain == Timeout) ? 0xFF : 0x00;

    // Set GOP Draw Color
    Color.Red = Color.Green = Color.Blue = DrawColor;

    // Print Combo Message
    AsciiPrintXY (XPos, YPos, &Color, NULL, ComboMessage);
  }
}

VOID
EFIAPI
PlatformBootManagerUnableToBoot ()
{
  EFI_STATUS Status;

  // Execute Secondary Unable To Boot
  DeviceBootManagerUnableToBoot ();

  // Display No Boot OS Logo
  Status = DisplayBootGraphic (BG_NO_BOOT_OS);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Display No Boot OS Logo! Status = %r\n", __FUNCTION__, Status));
    Print (L"No Operating System Found! Please make sure that an OS is Installed.\n");
  }

  // Wait 10s
  gBS->Stall (10000000);

  // Shutdown
  gRT->ResetSystem (EfiResetShutdown, EFI_SUCCESS, 0, NULL);

  // Do Cpu Dead Loop
  CpuDeadLoop ();
}

VOID
EFIAPI
PlatformBootManagerBdsEntry ()
{
  EFI_STATUS Status;

  // Setup Secure Boot
  Status = SetupSecureBoot ();
  ASSERT_EFI_ERROR (Status);

  // Update ACPI Tables
  UpdateAcpiTables ();

  // Execute Secondary BDS Entry
  DeviceBootManagerBdsEntry ();
}
