/** @file
  Runtime part of MmpTimerLib (TimerLib instance for DXE_RUNTIME_DRIVER modules).

  Every module links its own copy of MmpTimerLib, with its own mTimerBase. Each runtime module
  (RealTimeClockRuntimeDxe: GetTime reads the counter; VariableRuntimeDxe; ResetSystemRuntimeDxe)
  must move that copy to the virtual address the OS gives timer 0 in SetVirtualAddressMap.
  Without this, the first GetTime after the OS took over read - and wrote, to latch the counter -
  the physical address 0xD4014000 in the OS's address space.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeLib.h>
#include <Guid/EventGroup.h>
#include <Library/MmpTimerRuntime.h>
#include <Pxa988.h>

STATIC EFI_EVENT  mMmpTimerVirtualEvent;

STATIC
VOID
EFIAPI
MmpTimerVirtualAddressChange (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UINTN  Base;

  Base = PXA988_TIMER0_BASE;
  EfiConvertPointer (0, (VOID **)&Base);
  MmpTimerSetRuntimeBase (Base);
}

EFI_STATUS
EFIAPI
MmpTimerRuntimeLibConstructor (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  // Make sure the OS maps timer 0 for runtime services (idempotent with Pxa988ResetSystemLib)
  gDS->AddMemorySpace (EfiGcdMemoryTypeMemoryMappedIo, PXA988_TIMER0_BASE, SIZE_4KB, EFI_MEMORY_UC | EFI_MEMORY_RUNTIME | EFI_MEMORY_XP);
  gDS->SetMemorySpaceAttributes (PXA988_TIMER0_BASE, SIZE_4KB, EFI_MEMORY_UC | EFI_MEMORY_RUNTIME | EFI_MEMORY_XP);

  return gBS->CreateEventEx (
                EVT_NOTIFY_SIGNAL,
                TPL_NOTIFY,
                MmpTimerVirtualAddressChange,
                NULL,
                &gEfiEventVirtualAddressChangeGuid,
                &mMmpTimerVirtualEvent
                );
}
