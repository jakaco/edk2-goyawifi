/** @file
  ResetSystemLib for Marvell PXA988 / PXA986.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/ResetSystemLib.h>
#include <Library/TimerLib.h>
#include <Pxa988.h>

//
// PXA I2C (TWSI) registers, MMP layout
//
#define I2C_IDBR  0x08
#define I2C_ICR   0x10
#define I2C_ISR   0x18

#define ICR_START   BIT0
#define ICR_STOP    BIT1
#define ICR_ACKNAK  BIT2
#define ICR_TB      BIT3
#define ICR_SCLE    BIT5
#define ICR_IUE     BIT6

#define ISR_ITE  BIT6
#define ISR_IRF  BIT7

STATIC
EFI_STATUS
I2cWaitIsr (
  IN UINT32  Bit
  )
{
  UINTN  Timeout;

  for (Timeout = 0; Timeout < 10000; Timeout++) {
    if ((MmioRead32 (PXA988_TWSI2_BASE + I2C_ISR) & Bit) != 0) {
      MmioWrite32 (PXA988_TWSI2_BASE + I2C_ISR, Bit);
      return EFI_SUCCESS;
    }

    MicroSecondDelay (10);
  }

  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
I2cXfer (
  IN UINT8  Data,
  IN UINT32 Flags,
  IN UINT32 WaitBit
  )
{
  MmioWrite32 (PXA988_TWSI2_BASE + I2C_IDBR, Data);
  MmioWrite32 (PXA988_TWSI2_BASE + I2C_ICR, ICR_IUE | ICR_SCLE | ICR_TB | Flags);
  return I2cWaitIsr (WaitBit);
}

STATIC
EFI_STATUS
Pm822Write (
  IN UINT8  Reg,
  IN UINT8  Val
  )
{
  EFI_STATUS  Status;

  Status = I2cXfer (PM822_I2C_ADDR << 1, ICR_START, ISR_ITE);
  if (!EFI_ERROR (Status)) {
    Status = I2cXfer (Reg, 0, ISR_ITE);
  }

  if (!EFI_ERROR (Status)) {
    Status = I2cXfer (Val, ICR_STOP, ISR_ITE);
  }

  return Status;
}

STATIC
EFI_STATUS
Pm822Read (
  IN  UINT8  Reg,
  OUT UINT8  *Val
  )
{
  EFI_STATUS  Status;

  Status = I2cXfer (PM822_I2C_ADDR << 1, ICR_START, ISR_ITE);
  if (!EFI_ERROR (Status)) {
    Status = I2cXfer (Reg, 0, ISR_ITE);
  }

  if (!EFI_ERROR (Status)) {
    Status = I2cXfer ((PM822_I2C_ADDR << 1) | 1, ICR_START, ISR_ITE);
  }

  if (!EFI_ERROR (Status)) {
    MmioWrite32 (
      PXA988_TWSI2_BASE + I2C_ICR,
      ICR_IUE | ICR_SCLE | ICR_TB | ICR_STOP | ICR_ACKNAK
      );
    Status = I2cWaitIsr (ISR_IRF);
    *Val   = (UINT8)MmioRead32 (PXA988_TWSI2_BASE + I2C_IDBR);
  }

  return Status;
}

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

  // Make sure the TWSI2 clock is running (S-Boot normally leaves it on)
  MmioAndThenOr32 (APBCP_TWSI2, ~(UINT32)APBC_RST, APBC_APBCLK | APBC_FNCLK);

  Pm822Write (PM822_GENERAL_USE_REG, PM822_SHUTDOWN_BY_POWEROFF);
  if (!EFI_ERROR (Pm822Read (PM800_WAKEUP1, &Val))) {
    Pm822Write (PM800_WAKEUP1, Val | PM800_SW_PDOWN);
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
