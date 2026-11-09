/**
  Copyright (c) 2012-2013, ARM Limited. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/EvergoTrace.h>

#include "Sec.h"

//
// evergo Test-D: capture the full exception context into the beacon area
// before dead-looping (see EVERGO-PROJECT-STATUS-AUDIT-2026-11-05.md T1).
// ESR/FAR/SCTLR/TTBR0/TCR are exception-level dependent, so select the
// bank based on CurrentEL; everything else is bank-independent here
// because ELR/SP were captured by Exception.S before this call.
//
STATIC
VOID
SecReadFaultContext (
  OUT UINTN *Esr,
  OUT UINTN *Far,
  OUT UINTN *Sctlr,
  OUT UINTN *Ttbr0,
  OUT UINTN *Tcr,
  OUT UINTN *ElDaif
  )
{
  UINT64 CurrentEl;
  UINT64 Daif;

  asm volatile ("mrs %0, CurrentEL" : "=r" (CurrentEl));

  if ((CurrentEl & 0xC) == 0xC) {
    asm volatile ("mrs %0, esr_el2"   : "=r" (*Esr));
    asm volatile ("mrs %0, far_el2"   : "=r" (*Far));
    asm volatile ("mrs %0, sctlr_el2" : "=r" (*Sctlr));
    asm volatile ("mrs %0, ttbr0_el2" : "=r" (*Ttbr0));
    asm volatile ("mrs %0, tcr_el2"   : "=r" (*Tcr));
  } else {
    asm volatile ("mrs %0, esr_el1"   : "=r" (*Esr));
    asm volatile ("mrs %0, far_el1"   : "=r" (*Far));
    asm volatile ("mrs %0, sctlr_el1" : "=r" (*Sctlr));
    asm volatile ("mrs %0, ttbr0_el1" : "=r" (*Ttbr0));
    asm volatile ("mrs %0, tcr_el1"   : "=r" (*Tcr));
  }

  asm volatile ("mrs %0, daif" : "=r" (Daif));
  *ElDaif = (CurrentEl << 8) | (Daif & 0xFF);
}

VOID
SecCommonExceptionEntry (
  IN UINT32 Type,
  IN UINTN  LR,
  IN UINTN  SP,
  IN UINTN  OrigX0,
  IN UINTN  OrigX1,
  IN UINTN  OrigX2,
  IN UINTN  OrigX3)
{
  UINTN Esr;
  UINTN Far;
  UINTN Sctlr;
  UINTN Ttbr0;
  UINTN Tcr;
  UINTN ElDaif;

  CHAR8 Buffer[100];
  UINTN CharCount;

  SecReadFaultContext (&Esr, &Far, &Sctlr, &Ttbr0, &Tcr, &ElDaif);

  // Structured snapshot into the beacon area (marker 0xEE in slot 27, the
  // last normal step in slot 1 is preserved), watchdog kicked, cache
  // cleaned - survives the reset for the stage1 module to read back.
  SecExceptionCapture (
    Type, LR, SP, Esr, Far, Sctlr, Ttbr0, Tcr, ElDaif,
    OrigX0, OrigX1, OrigX2, OrigX3
    );

  // Structured TRC1 record (stage 0x0601, copy A only - copies B/C may be the
  // very cause of this fault, touching them here would recurse).
  TrcException (
    TRC_EXCEPTION_SEC,
    Esr,
    Far,
    LR,
    SP
    );

  // Set Exception Message
  switch (Type) {
    case EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS:
      CharCount = AsciiSPrint (Buffer, sizeof (Buffer), "Synchronous Exception at 0x%llx\n", LR);
      break;

    case EXCEPT_AARCH64_IRQ:
      CharCount = AsciiSPrint (Buffer, sizeof (Buffer), "IRQ Exception at 0x%llx\n", LR);
      break;

    case EXCEPT_AARCH64_FIQ:
      CharCount = AsciiSPrint (Buffer, sizeof (Buffer), "FIQ Exception at 0x%llx\n", LR);
      break;

    case EXCEPT_AARCH64_SERROR:
      CharCount = AsciiSPrint (Buffer, sizeof (Buffer), "SError/Abort Exception at 0x%llx\n", LR);
      break;

    default:
      CharCount = AsciiSPrint (Buffer, sizeof (Buffer), "Unknown Exception at 0x%llx\n", LR);
      break;
  }

  // Show Exception Message (no-op when the FB handoff magic does not match)
  SerialPortWrite ((UINT8 *)Buffer, CharCount);

  // CPU Dead Loop - the armed 15 s watchdog performs the reset.
  CpuDeadLoop ();
}
