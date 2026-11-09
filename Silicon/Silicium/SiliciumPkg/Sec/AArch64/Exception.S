//
// Copyright (c) 2011-2021, Arm Limited. All rights reserved.
// SPDX-License-Identifier: BSD-2-Clause-Patent
//

#include <AsmMacroLib.h>

#include <AArch64/AArch64.h>

.text

//
// Exception Handler
//
// Preserves the interrupted x0..x3 into x6..x9 BEFORE clobbering anything,
// then calls SecCommonExceptionEntry with:
//   x0 = exception class, x1 = faulting PC (ELR), x2 = interrupted SP,
//   x3..x6 = original x0..x3 (e.g. CreateHob's PHIT pointer and arguments).
// ESR/FAR are read by the C handler (exception-level dependent).
//
#define TO_HANDLER                               \
  mov  x6, x0                                   ;\
  mov  x7, x1                                   ;\
  mov  x8, x2                                   ;\
  mov  x9, x3                                   ;\
  EL1_OR_EL2 (x1)                               ;\
1:mrs  x1, elr_el1                             ;\
  b    3f                                      ;\
2:mrs  x1, elr_el2                             ;\
3:mov  x2, sp                                 ;\
  mov  x3, x6                                  ;\
  mov  x4, x7                                  ;\
  mov  x5, x8                                  ;\
  mov  x6, x9                                  ;\
  bl   ASM_PFX (SecCommonExceptionEntry)        ;

//
// Excetion Vector
//
#define EXCEPTION_VECTOR                                  \
VECTOR_ENTRY (SecVectorTable, ARM_VECTOR_CUR_SP0_SYNC)    \
_DefaultSyncExceptHandler_t:                              \
  mov   x0, #EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS       ;\
  TO_HANDLER                                             ;\
                                                          \
VECTOR_ENTRY (SecVectorTable, ARM_VECTOR_CUR_SP0_IRQ)     \
_DefaultIrq_t:                                            \
  mov   x0, #EXCEPT_AARCH64_IRQ                          ;\
  TO_HANDLER                                             ;\
                                                          \
VECTOR_ENTRY (SecVectorTable, ARM_VECTOR_CUR_SP0_FIQ)     \
_DefaultFiq_t:                                            \
  mov   x0, #EXCEPT_AARCH64_FIQ                          ;\
  TO_HANDLER                                             ;\
                                                          \
VECTOR_ENTRY (SecVectorTable, ARM_VECTOR_CUR_SP0_SERR)    \
_DefaultSError_t:                                         \
  mov   x0, #EXCEPT_AARCH64_SERROR                       ;\
  TO_HANDLER                                             ;\
                                                          \
VECTOR_ENTRY (SecVectorTable, ARM_VECTOR_CUR_SPX_SYNC)    \
_DefaultSyncExceptHandler_h:                              \
  mov   x0, #EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS       ;\
  TO_HANDLER                                             ;\
                                                          \
VECTOR_ENTRY (SecVectorTable, ARM_VECTOR_CUR_SPX_IRQ)     \
_DefaultIrq_h:                                            \
  mov   x0, #EXCEPT_AARCH64_IRQ                          ;\
  TO_HANDLER                                             ;\
                                                          \
VECTOR_ENTRY (SecVectorTable, ARM_VECTOR_CUR_SPX_FIQ)     \
_DefaultFiq_h:                                            \
  mov   x0, #EXCEPT_AARCH64_FIQ                          ;\
  TO_HANDLER                                             ;\
                                                          \
VECTOR_ENTRY (SecVectorTable, ARM_VECTOR_CUR_SPX_SERR)    \
_DefaultSError_h:                                         \
  mov   x0, #EXCEPT_AARCH64_SERROR                       ;\
  TO_HANDLER                                             ;\
                                                          \
VECTOR_ENTRY (SecVectorTable, ARM_VECTOR_LOW_A64_SYNC)    \
_DefaultSyncExceptHandler_LowerA64:                       \
  mov   x0, #EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS       ;\
  TO_HANDLER                                             ;\
                                                          \
VECTOR_ENTRY (SecVectorTable, ARM_VECTOR_LOW_A64_IRQ)     \
_DefaultIrq_LowerA64:                                     \
  mov   x0, #EXCEPT_AARCH64_IRQ                          ;\
  TO_HANDLER                                             ;\
                                                          \
VECTOR_ENTRY (SecVectorTable, ARM_VECTOR_LOW_A64_FIQ)     \
_DefaultFiq_LowerA64:                                     \
  mov   x0, #EXCEPT_AARCH64_FIQ                          ;\
  TO_HANDLER                                             ;\
                                                          \
VECTOR_ENTRY (SecVectorTable, ARM_VECTOR_LOW_A64_SERR)    \
_DefaultSError_LowerA64:                                  \
  mov   x0, #EXCEPT_AARCH64_SERROR                       ;\
  TO_HANDLER                                             ;\
                                                          \
VECTOR_ENTRY (SecVectorTable, ARM_VECTOR_LOW_A32_SYNC)    \
_DefaultSyncExceptHandler_LowerA32:                       \
  mov   x0, #EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS       ;\
  TO_HANDLER                                             ;\
                                                          \
VECTOR_ENTRY (SecVectorTable, ARM_VECTOR_LOW_A32_IRQ)     \
_DefaultIrq_LowerA32:                                     \
  mov   x0, #EXCEPT_AARCH64_IRQ                          ;\
  TO_HANDLER                                             ;\
                                                          \
VECTOR_ENTRY (SecVectorTable, ARM_VECTOR_LOW_A32_FIQ)     \
_DefaultFiq_LowerA32:                                     \
  mov   x0, #EXCEPT_AARCH64_FIQ                          ;\
  TO_HANDLER                                             ;\
                                                          \
VECTOR_ENTRY (SecVectorTable, ARM_VECTOR_LOW_A32_SERR)    \
_DefaultSError_LowerA32:                                  \
  mov   x0, #EXCEPT_AARCH64_SERROR                       ;\
  TO_HANDLER                                             ;

//
// Excetion Vector Table
//
VECTOR_TABLE (SecVectorTable, EXCEPTION_VECTOR)

AARCH64_BTI_NOTE ()
