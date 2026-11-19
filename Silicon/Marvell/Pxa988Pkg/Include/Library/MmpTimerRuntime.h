/** @file
  Runtime support for MmpTimerLib.

  A DXE runtime driver that uses TimerLib after ExitBootServices (the MMIO is
  reached through the virtual address the OS assigns in SetVirtualAddressMap)
  calls this from its virtual address change notification with the converted
  address of the timer 0 register block (PXA988_TIMER0_BASE).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef MMP_TIMER_RUNTIME_H_
#define MMP_TIMER_RUNTIME_H_

#include <Uefi.h>

VOID
EFIAPI
MmpTimerSetRuntimeBase (
  IN UINTN  VirtualBase
  );

#endif
