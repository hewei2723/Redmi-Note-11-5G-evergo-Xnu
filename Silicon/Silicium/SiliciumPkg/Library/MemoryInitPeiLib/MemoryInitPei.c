/**
  Copyright (c) 2011-2015, ARM Limited. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/PcdLib.h>
#include <Library/SortLib.h>
#include <Library/ArmMmuLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/MemoryMapLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>

// Implemented in Sec.c: bisect InitializeMemory()/MemoryPeim on real hardware.
VOID SecBeaconHook (IN UINTN Step);

VOID
BuildMemoryTypeInformationHob ();

STATIC
INTN
EFIAPI
CompareMemoryRegions (
  IN CONST VOID *P1,
  IN CONST VOID *P2)
{
  // Populate Memory Region Structure
  CONST EFI_MEMORY_REGION_DESCRIPTOR *Region1 = (EFI_MEMORY_REGION_DESCRIPTOR *)P1;
  CONST EFI_MEMORY_REGION_DESCRIPTOR *Region2 = (EFI_MEMORY_REGION_DESCRIPTOR *)P2;

  // Compare Base Addresses
  if (Region1->Address < Region2->Address) {
    return -1;
  }

  // Compare Base Addresses
  if (Region1->Address > Region2->Address) {
    return 1;
  }

  return 0;
}

STATIC
VOID
ValidateMemoryMap (
  IN OUT EFI_MEMORY_REGION_DESCRIPTOR *MemoryDescriptor,
  IN     UINT8                         MemoryDescriptorCount)
{
  BOOLEAN IsInvalid = FALSE;

  // Verify Memory Region Count
  ASSERT (MemoryDescriptorCount - 1 < MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT);

  // Sort Memory Regions
  PerformQuickSort (MemoryDescriptor, MemoryDescriptorCount, sizeof (EFI_MEMORY_REGION_DESCRIPTOR), CompareMemoryRegions);

  // Go thru each Memory Region
  for (UINT8 i = 0; i < MemoryDescriptorCount - 1; i++) {
    // Get Current & Next Memory Region
    EFI_MEMORY_REGION_DESCRIPTOR *CurrentRegion = &MemoryDescriptor[i];
    EFI_MEMORY_REGION_DESCRIPTOR *NextRegion    = &MemoryDescriptor[i + 1];

    // Calculate Current End Address
    EFI_PHYSICAL_ADDRESS EndAddress = CurrentRegion->Address + CurrentRegion->Length;

    // Skip Register Regions
    if (CurrentRegion->Address < FixedPcdGet64 (PcdSystemMemoryBase)) {
      continue;
    }

    // Check for Overlap
    if (EndAddress > NextRegion->Address) {
      DEBUG ((EFI_D_ERROR, "\"%a\" (0x%llx) and \"%a\" (0x%llx) Overlap!\n", CurrentRegion->Name, CurrentRegion->Address, NextRegion->Name, NextRegion->Address));

      // Set Memory Map State
      IsInvalid = TRUE;
    }
  }

  // Check Memory Map State
  if (IsInvalid) {
    CpuDeadLoop ();
  }
}

STATIC
VOID
AddHob (IN EFI_MEMORY_REGION_DESCRIPTOR MemoryRegion)
{
  // Save HOB Option
  EFI_MEMORY_HOB_OPTION HobOption = MemoryRegion.HobOption;

  // Build Resource Descriptor HOB
  if (HobOption == AddMem || HobOption == AddDev || HobOption == HobOnlyNoCacheSetting) {
    BuildResourceDescriptorHob (MemoryRegion.ResourceType, MemoryRegion.ResourceAttribute, MemoryRegion.Address, MemoryRegion.Length);
  }

  // Build Memory Allocation HOB
  if (MemoryRegion.ResourceType == EFI_RESOURCE_SYSTEM_MEMORY || MemoryRegion.MemoryType == EfiRuntimeServicesData) {
    BuildMemoryAllocationHob (MemoryRegion.Address, MemoryRegion.Length, MemoryRegion.MemoryType);
  }
}

EFI_STATUS
EFIAPI
MemoryPeim (
  IN EFI_PHYSICAL_ADDRESS UefiMemoryBase,
  IN UINT64               UefiMemorySize)
{
  EFI_STATUS                    Status                                              = EFI_SUCCESS;
  ARM_MEMORY_REGION_DESCRIPTOR  MemoryTable[MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT] = {0};
  EFI_MEMORY_REGION_DESCRIPTOR *MemoryDescriptor                                    = NULL;
  VOID                         *TranslationTableBase                                = NULL;
  UINTN                         TranslationTableSize                                = 0;
  UINT8                         MemoryDescriptorCount                               = 0;
  UINTN                         Index                                               = 0;

  SecBeaconHook (0x50);
  // Show Entry Message
  DEBUG ((EFI_D_WARN, "MMU In\r"));

  // Get Memory Map
  GetMemoryMap (&MemoryDescriptor, &MemoryDescriptorCount);
  SecBeaconHook (0x51);
  DEBUG ((EFI_D_WARN, "MMU map %u\n", MemoryDescriptorCount));

  // Validate Memory Map
  ValidateMemoryMap (MemoryDescriptor, MemoryDescriptorCount);
  SecBeaconHook (0x52);
  DEBUG ((EFI_D_WARN, "MMU sorted\n"));

  // Go thru each Memory Region
  for (UINT8 i = 0; i < MemoryDescriptorCount; i++) {
    // Add HOB
    AddHob (MemoryDescriptor[i]);

    // Verify Memory Region HOB Option
    if (MemoryDescriptor[i].HobOption == HobOnlyNoCacheSetting) {
      continue;
    }

    // Save Attributes
    MemoryTable[Index].PhysicalBase = MemoryDescriptor[i].Address;
    MemoryTable[Index].VirtualBase  = MemoryDescriptor[i].Address;
    MemoryTable[Index].Length       = MemoryDescriptor[i].Length;
    MemoryTable[Index].Attributes   = MemoryDescriptor[i].ArmAttributes;

    // Increase Memory Table Index
    Index++;
  }
  DEBUG ((EFI_D_WARN, "MMU table %u\n", Index));

  SecBeaconHook (0x53);
  // Configure MMU
  Status = ArmConfigureMmu (MemoryTable, &TranslationTableBase, &TranslationTableSize);
  SecBeaconHook (0x54);
  DEBUG ((EFI_D_WARN, "MMU config returned %r\n", Status));
  SecBeaconHook (0x55);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Configure MMU! Status = %r\n", Status));
    return Status;
  }

  // Build Memory Type Info HOB
  SecBeaconHook (0x56);
  if (FeaturePcdGet (PcdPrePiProduceMemoryTypeInformationHob)) {
    SecBeaconHook (0x58);
    BuildMemoryTypeInformationHob ();
    SecBeaconHook (0x59);
  }
  SecBeaconHook (0x57);

  return EFI_SUCCESS;
}
