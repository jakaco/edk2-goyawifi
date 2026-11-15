//
//  SPDX-License-Identifier: BSD-2-Clause-Patent
//

#include <AsmMacroLib.h>

//
// Runs first thing in SEC, before any stack exists. S-Boot enters the
// "kernel" with r0 = 0, r1 = machine id, r2 = ATAGs; we do not need them.
// Make sure we are in SVC mode with IRQ/FIQ masked.
//
ASM_FUNC(ArmPlatformPeiBootAction)
  mrs   r0, cpsr
  bic   r0, r0, #0x1f
  orr   r0, r0, #0xd3       // SVC, IRQ + FIQ disabled
  msr   cpsr_c, r0
  bx    lr
