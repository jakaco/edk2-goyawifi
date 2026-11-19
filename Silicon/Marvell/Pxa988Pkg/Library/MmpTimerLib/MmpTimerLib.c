/** @file
  TimerLib for Marvell PXA988 using MMP APB timer 0, counter 1.

  The counter cannot be read directly (metastability, see PXA168 manual);
  a read is requested through CVWR and the latched value read back after a
  few bus cycles, as done by Linux arch/arm/mach-mmp/time.c.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Base.h>
#include <Library/BaseLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Pxa988.h>
#include <Library/MmpTimerRuntime.h>

#define MMP_CNT  1

//
// The timer base is a variable so that a runtime driver can move it to the
// virtual address the OS gave the MMIO region (see MmpTimerRuntime.h). In
// runtime mode the timer is never (re)initialised: the OS owns timer 0 then.
//
STATIC UINTN    mTimerBase   = PXA988_TIMER0_BASE;
STATIC BOOLEAN  mRuntimeMode = FALSE;

VOID
EFIAPI
MmpTimerSetRuntimeBase (
  IN UINTN  VirtualBase
  )
{
  mTimerBase   = VirtualBase;
  mRuntimeMode = TRUE;
}

STATIC
VOID
MmpTimerEnsureRunning (
  VOID
  )
{
  UINT32  Cer;

  if (mRuntimeMode) {
    return;
  }

  Cer = MmioRead32 (mTimerBase + TMR_CER);
  if ((Cer & (1U << MMP_CNT)) != 0) {
    return;
  }

  //
  // 3.25 MHz functional clock (FNCLKSEL 3), out of reset
  //
  MmioWrite32 (APBC_TIMERS0, APBC_APBCLK | APBC_RST);
  MmioWrite32 (APBC_TIMERS0, APBC_APBCLK | APBC_FNCLK | APBC_FNCLKSEL (3));

  MmioAnd32 (mTimerBase + TMR_CCR, ~(UINT32)(3U << 2));   // counter 1: fast clock
  MmioOr32 (mTimerBase + TMR_CMR, 1U << MMP_CNT);         // free running
  MmioWrite32 (mTimerBase + TMR_PLCR (MMP_CNT), 0);
  MmioWrite32 (mTimerBase + TMR_ICR (MMP_CNT), 0x7);
  MmioWrite32 (mTimerBase + TMR_IER (MMP_CNT), 0);
  MmioWrite32 (mTimerBase + TMR_CER, Cer | (1U << MMP_CNT));
}

STATIC
UINT32
MmpTimerRead (
  VOID
  )
{
  UINT32  Val;
  UINTN   Delay;

  MmpTimerEnsureRunning ();

  MmioWrite32 (mTimerBase + TMR_CVWR (MMP_CNT), 1);
  Val = 0;
  for (Delay = 0; Delay < 3; Delay++) {
    Val = MmioRead32 (mTimerBase + TMR_CVWR (MMP_CNT));
  }

  return Val;
}

STATIC
VOID
MmpTimerWaitTicks (
  IN UINT64  Ticks
  )
{
  UINT32  Last;
  UINT32  Now;

  Last = MmpTimerRead ();
  while (Ticks > 0) {
    Now = MmpTimerRead ();
    if ((UINT32)(Now - Last) >= Ticks) {
      break;
    }

    Ticks -= (UINT32)(Now - Last);
    Last   = Now;
  }
}

UINTN
EFIAPI
MicroSecondDelay (
  IN UINTN  MicroSeconds
  )
{
  // 3.25 ticks per microsecond
  MmpTimerWaitTicks (DivU64x32 (MultU64x32 (MicroSeconds, 13), 4) + 1);
  return MicroSeconds;
}

UINTN
EFIAPI
NanoSecondDelay (
  IN UINTN  NanoSeconds
  )
{
  MicroSecondDelay ((NanoSeconds + 999) / 1000);
  return NanoSeconds;
}

UINT64
EFIAPI
GetPerformanceCounter (
  VOID
  )
{
  return MmpTimerRead ();
}

UINT64
EFIAPI
GetPerformanceCounterProperties (
  OUT UINT64  *StartValue  OPTIONAL,
  OUT UINT64  *EndValue    OPTIONAL
  )
{
  if (StartValue != NULL) {
    *StartValue = 0;
  }

  if (EndValue != NULL) {
    *EndValue = 0xFFFFFFFF;
  }

  return PXA988_TIMER_FREQ;
}

UINT64
EFIAPI
GetTimeInNanoSecond (
  IN UINT64  Ticks
  )
{
  // Ticks * 1e9 / 3.25e6 = Ticks * 4000 / 13
  return DivU64x32 (MultU64x32 (Ticks, 4000), 13);
}
