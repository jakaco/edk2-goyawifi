// Debug register probes for GoyaWifiDiag: one instruction each, in ARM state.
// An Undefined Instruction exception is caught by the diag app's handler, which
// returns to the caller (lr) with r0 = 0xDEADC0DE.
// SPDX-License-Identifier: BSD-2-Clause-Patent
  .syntax unified
  .arm
  .text
  .align 2
  .global GoyaDbgIdDfr0
  .type GoyaDbgIdDfr0, %function
GoyaDbgIdDfr0:
  mrc p15, 0, r0, c0, c1, 2
  nop
  bx lr
  .global GoyaDbgDidr
  .type GoyaDbgDidr, %function
GoyaDbgDidr:
  mrc p14, 0, r0, c0, c0, 0
  nop
  bx lr
  .global GoyaDbgDrar
  .type GoyaDbgDrar, %function
GoyaDbgDrar:
  mrc p14, 0, r0, c1, c0, 0
  nop
  bx lr
  .global GoyaDbgDsar
  .type GoyaDbgDsar, %function
GoyaDbgDsar:
  mrc p14, 0, r0, c2, c0, 0
  nop
  bx lr
  .global GoyaDbgOslsr
  .type GoyaDbgOslsr, %function
GoyaDbgOslsr:
  mrc p14, 0, r0, c1, c1, 4
  nop
  bx lr
  .global GoyaDbgPrsr
  .type GoyaDbgPrsr, %function
GoyaDbgPrsr:
  mrc p14, 0, r0, c1, c5, 4
  nop
  bx lr
  .global GoyaDbgOslarW
  .type GoyaDbgOslarW, %function
GoyaDbgOslarW:
  mcr p14, 0, r0, c1, c0, 4
  isb
  bx lr
  .global GoyaDbgDscrInt
  .type GoyaDbgDscrInt, %function
GoyaDbgDscrInt:
  mrc p14, 0, r0, c0, c1, 0
  nop
  bx lr
  .global GoyaDbgDscrExt
  .type GoyaDbgDscrExt, %function
GoyaDbgDscrExt:
  mrc p14, 0, r0, c0, c2, 2
  nop
  bx lr
  .global GoyaDbgDscrExtW
  .type GoyaDbgDscrExtW, %function
GoyaDbgDscrExtW:
  mcr p14, 0, r0, c0, c2, 2
  isb
  bx lr
  .global GoyaDbgVcrW
  .type GoyaDbgVcrW, %function
GoyaDbgVcrW:
  mcr p14, 0, r0, c0, c7, 0
  isb
  bx lr
  .global GoyaDbgBcr0
  .type GoyaDbgBcr0, %function
GoyaDbgBcr0:
  mrc p14, 0, r0, c0, c0, 5
  nop
  bx lr
  .global GoyaDbgBcr0W
  .type GoyaDbgBcr0W, %function
GoyaDbgBcr0W:
  mcr p14, 0, r0, c0, c0, 5
  isb
  bx lr
  .global GoyaDbgBvr0W
  .type GoyaDbgBvr0W, %function
GoyaDbgBvr0W:
  mcr p14, 0, r0, c0, c0, 4
  isb
  bx lr
  .global GoyaDbgWcr0W
  .type GoyaDbgWcr0W, %function
GoyaDbgWcr0W:
  mcr p14, 0, r0, c0, c0, 7
  isb
  bx lr
  .global GoyaDbgWfar
  .type GoyaDbgWfar, %function
GoyaDbgWfar:
  mrc p14, 0, r0, c0, c6, 0
  nop
  bx lr
