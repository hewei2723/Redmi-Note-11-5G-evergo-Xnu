//
// Copyright (c) 2011 - 2025, Arm Limited. All rights reserved.
// SPDX-License-Identifier: BSD-2-Clause-Patent
//

#include <AsmMacroLib.h>

#include <Library/ArmLib.h>

//
// SCTRL_EL1 Bits
//
.set CTRL_SPAN_BIT, BIT23

//
// Test-D beacon: write a step marker into the DTB-reserved payload slot
// (0xB5400000 + step*4). Runs with the MMU off, so stores hit DRAM
// directly and survive the reset for the stage1 module to read back.
//
.macro SEC_BEACON val
  movz  x9, #0x4811, lsl #16
  movk  x9, #0xFD00
  movz  w10, #\val
  str   w10, [x9, #(\val * 4)]
  dsb   sy
.endm

ASM_FUNC (_ModuleEntryPoint)
  SEC_BEACON 1

  // Do Platform Specific Initialization
  bl    ASM_PFX (PlatformAssemblyInitialize)
  SEC_BEACON 2

  // Disable SPAN
  EL1_OR_EL2 (x0)
1:mrs   x0, sctlr_el1
  orr   x0, x0, #CTRL_SPAN_BIT
  msr   sctlr_el1, x0
2:nop
  SEC_BEACON 3

  // Enable Floating Point
  bl    ASM_PFX (ArmEnableVFP)
  SEC_BEACON 4

  // Setup Exception Vector
  EL1_OR_EL2 (x0)
1:bl    ASM_PFX (SetupExceptionLevel1)
  b     ASM_PFX (MainEntryPoint)
2:bl    ASM_PFX (SetupExceptionLevel2)
  b     ASM_PFX (MainEntryPoint)

ASM_PFX (MainEntryPoint):
  SEC_BEACON 5
  // Get Stack Details
  MOV64 (x0, FixedPcdGet64 (PcdCPUCoresStackBase))
  MOV32 (x1, FixedPcdGet32 (PcdCPUCorePrimaryStackSize))

  // Set Stack End Address
  add   x2, x0, x1

  // Set Stack Pointer
  mov   sp, x2

  // Clear Stack
  mov   x3, xzr
0:stp   x3, x3, [x0], #16
  cmp   x0, x2
  b.lt  0b
  SEC_BEACON 6

  // Set Jump Location
  ldr   x0, =ASM_PFX (SecEntry)

  // Clear Frame Pointer
  mov   x29, xzr

  // Jump to C Code
  blr   x0

  // Do Infinite Spin
  b     .
