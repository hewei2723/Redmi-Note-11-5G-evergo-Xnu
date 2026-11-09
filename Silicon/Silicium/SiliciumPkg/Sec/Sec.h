/**
  Copyright (c) 2011 - 2020, Arm Limited. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef _SEC_H_
#define _SEC_H_

VOID
SecVectorTable ();

//
// Called from Exception.S with the faulting PC (ELR), the interrupted SP,
// and the interrupted x0..x3 (e.g. CreateHob's PHIT pointer and arguments).
// Reads the remaining context itself (ESR/FAR/SCTLR/TTBR0/TCR/CurrentEL/
// DAIF), records a structured snapshot into the Test-D beacon area WITHOUT
// overwriting the last normal progress step (slot 1), then the caller
// dead-loops so the watchdog resets and the snapshot survives.
//
VOID
SecCommonExceptionEntry (
  IN UINT32 Type,
  IN UINTN  LR,
  IN UINTN  SP,
  IN UINTN  OrigX0,
  IN UINTN  OrigX1,
  IN UINTN  OrigX2,
  IN UINTN  OrigX3
  );

//
// Defined in Sec.c: writes the structured exception snapshot into the
// Test-D beacon area (slots 18..31; slot 27 carries 0xEE plus the last
// normal step so a fault never erases progress evidence).
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
  );

EFI_STATUS
EFIAPI
MemoryPeim (
  IN EFI_PHYSICAL_ADDRESS  UefiMemoryBase,
  IN UINT64                UefiMemorySize
  );

EFI_STATUS
EFIAPI
PlatformPeim ();

VOID
EFIAPI
ProcessLibraryConstructorList ();

#endif /* _SEC_H_ */
