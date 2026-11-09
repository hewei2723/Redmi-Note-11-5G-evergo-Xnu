##
#  Copyright (c) 2011 - 2022, ARM Limited. All rights reserved.
#  Copyright (c) 2014, Linaro Limited. All rights reserved.
#  Copyright (c) 2015 - 2020, Intel Corporation. All rights reserved.
#  Copyright (c) 2018, Bingxing Wang. All rights reserved.
#  Copyright (c) Microsoft Corporation.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

################################################################################
#
# Defines Section - statements that will be processed to create a Makefile.
#
################################################################################
[Defines]
  PLATFORM_NAME                  = evergo
  PLATFORM_GUID                  = 111182d0-d1c9-43b6-8605-7a6ee449098d
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/evergoPkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = evergoPkg/evergo.fdf
  USE_CUSTOM_DISPLAY_DRIVER      = 0

!include MT6833Pkg/MT6833Pkg.dsc.inc

[PcdsFixedAtBuild]
  #
  # Silence every DEBUG() print.
  #
  # Bisected 2026-11-07 on the hot-jump path: the UEFI console (ConOut) write
  # at this stage hangs the machine - the trace stops exactly at the
  # "Loading driver at ..." DEBUG in CoreLoadPeImage and the watchdog resets
  # us. The console itself is fine (the Project Silicium banner renders), so
  # the hazard is in the DEBUG->ConOut path, not in drawing.
  # TRC (0x4811F800 / 0xA4000800) and the beacon page remain as channels.
  #
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x00000000

  #
  # DDR Memory
  #
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x40000000
  gArmTokenSpaceGuid.PcdSystemMemorySize|0x1C0000000

  #
  # UEFI Stack
  #
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0xB0200000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x40000

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"Xiaomi"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"Redmi Note 11 5G"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"evergo"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"Redmi_Note_10_5G_evergo"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"Redmi Note 11 5G"

  #
  # Simple Frame Buffer
  #
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|1088
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|2400
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32

  #
  # Storage
  #
  # UFS support is not implemented yet; do not publish an eMMC block device.
  gMediaTekPkgTokenSpaceGuid.PcdStorageIsEMMC|FALSE

[LibraryClasses]
  #
  # Memory Libraries
  #
  MemoryMapLib|evergoPkg/Library/MemoryMapLib/MemoryMapLib.inf

  #
  # Input Libraries
  #
  KeypadDeviceLib|evergoPkg/Library/KeypadDeviceLib/KeypadDeviceLib.inf

  UniXNULib|UniXNUPkg/Library/UniXNULib/UniXNULib.inf

[Components]
  #
  # Early handoff and payload publication
  #
  evergoPkg/Drivers/CamelliaBootMemoryDxe/CamelliaBootMemoryDxe.inf
  evergoPkg/Drivers/CamelliaRamDiskDxe/CamelliaRamDiskDxe.inf
  evergoPkg/Drivers/CamelliaUniXNULoaderDxe/CamelliaUniXNULoaderDxe.inf
  evergoPkg/Drivers/EvergoBootFromRamDiskDxe/EvergoBootFromRamDiskDxe.inf

  #
  # UniXNU boot manager (2026-11-09 Stage-2 integration): the menu application
  # and the FDT-initrd RAM disk publisher. UniXNUPkg is vendored under
  # evergoPkg/UniXNUPkg (from UniXNU/UniXNU package/, BSD-2-Clause).
  #
  UniXNUPkg/Driver/UniXNURamDiskDxe/UniXNURamDiskDxe.inf
  UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf
  UniXNUPkg/Application/UniXNU/UniXNU.inf

  #
  # ACPI Tables
  #
  # Placeholder until a evergo ACPI platform is derived from the stock DTB.
  lancelot/AcpiTables.inf

  #
  # Input
  #
  SiliciumPkg/Drivers/KeypadDxe/KeypadDxe.inf
  SiliciumPkg/Drivers/KeypadDeviceDxe/KeypadDeviceDxe.inf

[BuildOptions]
  # evergo Test-D: enable the DXE-phase beacon code in shared Mu files
  # (DxeMain.c); the SEC-side beacons are unconditional evergo-tree files.
  *_*_*_CC_FLAGS = -DEVERGO_SEC_BEACONS
