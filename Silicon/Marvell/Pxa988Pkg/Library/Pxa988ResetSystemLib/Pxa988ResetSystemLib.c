/** @file
  ResetSystemLib for Marvell PXA988 / PXA986.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/Pm822Lib.h>
#include <Library/ResetSystemLib.h>
#include <Library/TimerLib.h>
#include <Pxa988.h>

STATIC
VOID
Pxa988WatchdogReset (
  VOID
  )
{
  UINT32  Val;

  // Hold the CP and reset the MSA so they cannot cancel the WDT reset
  MmioOr32 (MPMU_APRR, MPMU_APRR_CPR);
  MicroSecondDelay (10);
  MmioOr32 (MPMU_CPRR, BIT2 | BIT3);
  MicroSecondDelay (10);

  // Reset and enable the watchdog clock
  MmioWrite32 (MPMU_WDTPCR, 0x7);
  MmioRead32 (MPMU_WDTPCR);
  MmioWrite32 (MPMU_WDTPCR, 0x3);
  MmioRead32 (MPMU_WDTPCR);

  MmioWrite32 (WDT_WFAR, 0xBABA);
  MmioWrite32 (WDT_WSAR, 0xEB10);
  MmioWrite32 (WDT_WMER, 0x3);

  MmioOr32 (MPMU_APRR, MPMU_APRR_WDTR);

  MmioWrite32 (WDT_WFAR, 0xBABA);
  MmioWrite32 (WDT_WSAR, 0xEB10);
  MmioWrite32 (WDT_WSR, 0);

  Val = MmioRead32 (WDT_WVR);
  MmioWrite32 (WDT_WFAR, 0xBABA);
  MmioWrite32 (WDT_WSAR, 0xEB10);
  MmioWrite32 (WDT_WMR, (Val + 0x20) & 0xFFFF);

  CpuDeadLoop ();
}

VOID
EFIAPI
ResetCold (
  VOID
  )
{
  Pxa988WatchdogReset ();
}

VOID
EFIAPI
ResetWarm (
  VOID
  )
{
  Pxa988WatchdogReset ();
}

VOID
EFIAPI
ResetShutdown (
  VOID
  )
{
  UINT8  Val;

  Pm822Write (PM822_I2C_ADDR, PM822_GENERAL_USE_REG, PM822_SHUTDOWN_BY_POWEROFF);
  if (!EFI_ERROR (Pm822Read (PM822_I2C_ADDR, PM800_WAKEUP1, &Val))) {
    Pm822Write (PM822_I2C_ADDR, PM800_WAKEUP1, Val | PM800_SW_PDOWN);
  }

  MicroSecondDelay (1000000);
  DEBUG ((DEBUG_ERROR, "%a: still powered, resetting\n", __func__));
  Pxa988WatchdogReset ();
}

VOID
EFIAPI
ResetPlatformSpecific (
  IN UINTN  DataSize,
  IN VOID   *ResetData
  )
{
  //
  // ResetData starts with a NUL terminated CHAR16 string; "recovery" makes
  // S-Boot boot the RECOVERY partition after the reset.
  //
  if ((ResetData != NULL) && (DataSize >= sizeof (L"recovery")) &&
      (StrnCmp ((CHAR16 *)ResetData, L"recovery", 8) == 0))
  {
    MmioWrite32 (PXA988_RTC_BR0, RTC_BR0_RECOVERY);
  }

  Pxa988WatchdogReset ();
}

VOID
EFIAPI
ResetSystem (
  IN EFI_RESET_TYPE  ResetType,
  IN EFI_STATUS      ResetStatus,
  IN UINTN           DataSize,
  IN VOID            *ResetData OPTIONAL
  )
{
  switch (ResetType) {
    case EfiResetWarm:
      ResetWarm ();
      break;
    case EfiResetCold:
      ResetCold ();
      break;
    case EfiResetShutdown:
      ResetShutdown ();
      return;
    case EfiResetPlatformSpecific:
      ResetPlatformSpecific (DataSize, ResetData);
      return;
    default:
      return;
  }
}
