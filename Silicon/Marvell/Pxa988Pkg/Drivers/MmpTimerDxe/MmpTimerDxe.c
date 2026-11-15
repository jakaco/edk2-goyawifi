/** @file
  Timer architectural protocol for Marvell PXA988 (MMP APB timer 0).

  Counter 1 of the same block is the free-running TimerLib counter and must
  keep running, so every CER write preserves bit 1.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/HardwareInterrupt.h>
#include <Protocol/Timer.h>
#include <Pxa988.h>

#define TMR(r)       (PXA988_TIMER0_BASE + (r))
#define CER_CNT0     BIT0
#define CER_CNT1     BIT1

STATIC EFI_TIMER_NOTIFY             mTimerNotifyFunction;
STATIC UINT64                       mTimerPeriod;       // 100 ns units
STATIC UINT32                       mTimerTicks;        // timer ticks per period
STATIC EFI_HARDWARE_INTERRUPT_PROTOCOL  *mInterrupt;
STATIC EFI_HANDLE                   mTimerHandle;

STATIC
VOID
MmpTimerArm (
  VOID
  )
{
  // Same sequence as Linux timer_set_next_event()
  MmioWrite32 (TMR (TMR_CER), CER_CNT1);
  MmioWrite32 (TMR (TMR_ICR (0)), 0x1);
  MmioWrite32 (TMR (TMR_IER (0)), 0x1);
  MmioWrite32 (TMR (TMR_TN_MM (0, 0)), mTimerTicks - 1);
  MmioWrite32 (TMR (TMR_CER), CER_CNT1 | CER_CNT0);
}

STATIC
VOID
MmpTimerStop (
  VOID
  )
{
  MmioWrite32 (TMR (TMR_IER (0)), 0);
  MmioWrite32 (TMR (TMR_CER), CER_CNT1);
  MmioWrite32 (TMR (TMR_ICR (0)), 0x1);
}

STATIC
VOID
EFIAPI
MmpTimerInterruptHandler (
  IN  HARDWARE_INTERRUPT_SOURCE  Source,
  IN  EFI_SYSTEM_CONTEXT         SystemContext
  )
{
  EFI_TPL  OriginalTPL;

  OriginalTPL = gBS->RaiseTPL (TPL_HIGH_LEVEL);

  MmioWrite32 (TMR (TMR_ICR (0)), 0x1);
  mInterrupt->EndOfInterrupt (mInterrupt, Source);

  if (mTimerPeriod != 0) {
    MmpTimerArm ();
  }

  if (mTimerNotifyFunction != NULL) {
    mTimerNotifyFunction (mTimerPeriod);
  }

  gBS->RestoreTPL (OriginalTPL);
}

STATIC
EFI_STATUS
EFIAPI
MmpTimerRegisterHandler (
  IN EFI_TIMER_ARCH_PROTOCOL  *This,
  IN EFI_TIMER_NOTIFY         NotifyFunction
  )
{
  if ((NotifyFunction == NULL) && (mTimerNotifyFunction == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((NotifyFunction != NULL) && (mTimerNotifyFunction != NULL)) {
    return EFI_ALREADY_STARTED;
  }

  mTimerNotifyFunction = NotifyFunction;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MmpTimerSetPeriod (
  IN EFI_TIMER_ARCH_PROTOCOL  *This,
  IN UINT64                   TimerPeriod
  )
{
  UINT64  Ticks;

  MmpTimerStop ();
  mTimerPeriod = TimerPeriod;

  if (TimerPeriod == 0) {
    return EFI_SUCCESS;
  }

  // TimerPeriod is in 100 ns units: ticks = period * 3.25 MHz / 10 MHz
  Ticks = DivU64x32 (MultU64x32 (TimerPeriod, PXA988_TIMER_FREQ / 1000), 10000);
  if (Ticks < 2) {
    Ticks = 2;
  } else if (Ticks > MAX_UINT32) {
    Ticks = MAX_UINT32;
  }

  mTimerTicks = (UINT32)Ticks;
  MmpTimerArm ();

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MmpTimerGetPeriod (
  IN  EFI_TIMER_ARCH_PROTOCOL  *This,
  OUT UINT64                   *TimerPeriod
  )
{
  if (TimerPeriod == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *TimerPeriod = mTimerPeriod;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MmpTimerGenerateSoftInterrupt (
  IN EFI_TIMER_ARCH_PROTOCOL  *This
  )
{
  return EFI_UNSUPPORTED;
}

STATIC EFI_TIMER_ARCH_PROTOCOL  mTimer = {
  MmpTimerRegisterHandler,
  MmpTimerSetPeriod,
  MmpTimerGetPeriod,
  MmpTimerGenerateSoftInterrupt
};

STATIC
VOID
EFIAPI
MmpTimerExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  // Leave counter 1 running for the OS, but stop our tick interrupt
  MmpTimerStop ();
}

EFI_STATUS
EFIAPI
MmpTimerInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_EVENT   ExitBootServicesEvent;

  Status = gBS->LocateProtocol (&gHardwareInterruptProtocolGuid, NULL, (VOID **)&mInterrupt);
  ASSERT_EFI_ERROR (Status);

  // Timer 0 match 0 periodic-with-preload, counter 1 free running
  MmioAnd32 (TMR (TMR_CCR), ~(UINT32)0x3);       // counter 0: fast clock
  MmioAnd32 (TMR (TMR_CMR), ~(UINT32)BIT0);       // counter 0: periodic
  MmioWrite32 (TMR (TMR_PLCR (0)), 0x1);
  MmpTimerStop ();

  Status = mInterrupt->RegisterInterruptSource (mInterrupt, PXA988_TIMER0_IRQ, MmpTimerInterruptHandler);
  ASSERT_EFI_ERROR (Status);

  Status = MmpTimerSetPeriod (&mTimer, PcdGet32 (PcdTimerPeriod));
  ASSERT_EFI_ERROR (Status);

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mTimerHandle,
                  &gEfiTimerArchProtocolGuid,
                  &mTimer,
                  NULL
                  );
  ASSERT_EFI_ERROR (Status);

  Status = gBS->CreateEvent (
                  EVT_SIGNAL_EXIT_BOOT_SERVICES,
                  TPL_NOTIFY,
                  MmpTimerExitBootServices,
                  NULL,
                  &ExitBootServicesEvent
                  );
  ASSERT_EFI_ERROR (Status);

  return Status;
}
