//
// Cpu1Stub.S - what the second Cortex-A9 core runs when released from reset.
//
// The BootROM starts the core at the physical address in CIU_WARM_RESET_VECTOR
// with MMU, caches and branch prediction off, in ARM state, SVC mode. The stub
// is position independent: Pxa988SmpDxe copies [Pxa988Cpu1StubStart,
// Pxa988Cpu1StubEnd) to a page of RAM, which it then points the vector at.
// Everything the stub touches lives in that page, so it never needs a cache
// or an MMU (all its accesses are strongly ordered).
//
// Mailbox (at +0x100 of the page, offsets match Pxa988SmpDxe.c). Words written
// by CPU1 and words written by CPU0 sit in different 32-byte cache lines, so
// CPU0 cleaning its line can never overwrite something CPU1 stored:
//   CPU1 -> CPU0:  +00 alive magic  +04 MIDR  +08 MPIDR  +0C SCTLR
//                  +10 heartbeat    +18 ack   +20 result +24 state (1 run, 2 parked)
//   CPU0 -> CPU1:  +40 command      +44 argument
//
// SPDX-License-Identifier: BSD-2-Clause-Patent
//

#define MB_ALIVE      0x00
#define MB_MIDR       0x04
#define MB_MPIDR      0x08
#define MB_SCTLR      0x0C
#define MB_HEARTBEAT  0x10
#define MB_CMD        0x40
#define MB_ACK        0x18
#define MB_ARG        0x44
#define MB_RESULT     0x20
#define MB_STATE      0x24

  .arm
  .text
  .align 5

  .globl  Pxa988Cpu1StubStart
Pxa988Cpu1StubStart:
  cpsid   aif, #0x13                // SVC, IRQ/FIQ/abort masked

  mov     r0, #0
  mcr     p15, 0, r0, c7, c5, 0     // invalidate I-cache
  mcr     p15, 0, r0, c7, c5, 6     // invalidate branch predictor
  mrc     p15, 0, r0, c1, c0, 0     // SCTLR: MMU, D-cache, I-cache, BP off
  bic     r0, r0, #0x1800
  bic     r0, r0, #0x5
  mcr     p15, 0, r0, c1, c0, 0
  isb

  adr     r4, Pxa988Cpu1Mailbox

  mrc     p15, 0, r0, c0, c0, 0
  str     r0, [r4, #MB_MIDR]
  mrc     p15, 0, r0, c0, c0, 5
  str     r0, [r4, #MB_MPIDR]
  mrc     p15, 0, r0, c1, c0, 0
  str     r0, [r4, #MB_SCTLR]
  mov     r0, #1
  str     r0, [r4, #MB_STATE]
  movw    r0, #0xC0DE
  movt    r0, #0xA9A9
  dsb
  str     r0, [r4, #MB_ALIVE]       // last: tells CPU0 we are up
  dsb
  sev

  mov     r5, #0                    // heartbeat
  mov     r6, #0                    // last command handled
Pxa988Cpu1Loop:
  add     r5, r5, #1
  str     r5, [r4, #MB_HEARTBEAT]
  dsb
  ldr     r0, [r4, #MB_CMD]
  cmp     r0, #1
  beq     Pxa988Cpu1Park            // command 1: park
  cmp     r0, #0
  cmpne   r0, r6
  beq     Pxa988Cpu1Wait            // nothing new
  mov     r6, r0                    // any other value: ping (ack with ~arg)
  ldr     r1, [r4, #MB_ARG]
  mvn     r1, r1
  str     r1, [r4, #MB_RESULT]
  dsb
  str     r0, [r4, #MB_ACK]
  dsb
  sev
Pxa988Cpu1Wait:
  wfe
  b       Pxa988Cpu1Loop

Pxa988Cpu1Park:
  mov     r0, #2
  str     r0, [r4, #MB_STATE]
  dsb
  sev
1:
  wfi
  b       1b

  .org    Pxa988Cpu1StubStart + 0x100
  .globl  Pxa988Cpu1Mailbox
Pxa988Cpu1Mailbox:
  .space  0x80
  .globl  Pxa988Cpu1StubEnd
Pxa988Cpu1StubEnd:
