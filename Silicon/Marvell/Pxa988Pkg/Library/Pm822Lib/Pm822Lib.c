/** @file
  Polled 88PM822 access over the PXA988 TWSI2 controller (PXA I2C layout).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/IoLib.h>
#include <Library/Pm822Lib.h>
#include <Library/TimerLib.h>
#include <Pxa988.h>

#define I2C_IDBR  0x08
#define I2C_ICR   0x10
#define I2C_ISR   0x18

#define ICR_START   BIT0
#define ICR_STOP    BIT1
#define ICR_ACKNAK  BIT2
#define ICR_TB      BIT3
#define ICR_SCLE    BIT5
#define ICR_IUE     BIT6

#define ISR_ACKNAK  BIT1
#define ISR_ITE     BIT6
#define ISR_IRF     BIT7

STATIC
VOID
I2cEnsureClock (
  VOID
  )
{
  // S-Boot talks to the PMIC, so this is normally already on
  if ((MmioRead32 (APBCP_TWSI2) & (APBC_APBCLK | APBC_FNCLK | APBC_RST)) !=
      (APBC_APBCLK | APBC_FNCLK))
  {
    MmioAndThenOr32 (APBCP_TWSI2, ~(UINT32)APBC_RST, APBC_APBCLK | APBC_FNCLK);
    MicroSecondDelay (10);
  }
}

STATIC
EFI_STATUS
I2cWaitIsr (
  IN UINT32  Bit
  )
{
  UINTN  Timeout;

  for (Timeout = 0; Timeout < 1000; Timeout++) {
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
  IN UINT8   Data,
  IN UINT32  Flags
  )
{
  EFI_STATUS  Status;

  MmioWrite32 (PXA988_TWSI2_BASE + I2C_IDBR, Data);
  MmioWrite32 (PXA988_TWSI2_BASE + I2C_ICR, ICR_IUE | ICR_SCLE | ICR_TB | Flags);
  Status = I2cWaitIsr (ISR_ITE);
  if (!EFI_ERROR (Status) &&
      ((MmioRead32 (PXA988_TWSI2_BASE + I2C_ISR) & ISR_ACKNAK) != 0))
  {
    Status = EFI_DEVICE_ERROR;      // NAK
  }

  return Status;
}

STATIC
VOID
I2cStop (
  VOID
  )
{
  MmioWrite32 (PXA988_TWSI2_BASE + I2C_ICR, ICR_IUE | ICR_SCLE | ICR_STOP);
}

EFI_STATUS
EFIAPI
Pm822Write (
  IN UINT8  Page,
  IN UINT8  Reg,
  IN UINT8  Val
  )
{
  EFI_STATUS  Status;

  I2cEnsureClock ();
  Status = I2cXfer (Page << 1, ICR_START);
  if (!EFI_ERROR (Status)) {
    Status = I2cXfer (Reg, 0);
  }

  if (!EFI_ERROR (Status)) {
    Status = I2cXfer (Val, ICR_STOP);
  }

  if (EFI_ERROR (Status)) {
    I2cStop ();
  }

  return Status;
}

EFI_STATUS
EFIAPI
Pm822Read (
  IN  UINT8  Page,
  IN  UINT8  Reg,
  OUT UINT8  *Val
  )
{
  EFI_STATUS  Status;

  I2cEnsureClock ();
  Status = I2cXfer (Page << 1, ICR_START);
  if (!EFI_ERROR (Status)) {
    Status = I2cXfer (Reg, 0);
  }

  if (!EFI_ERROR (Status)) {
    Status = I2cXfer ((Page << 1) | 1, ICR_START);
  }

  if (!EFI_ERROR (Status)) {
    MmioWrite32 (
      PXA988_TWSI2_BASE + I2C_ICR,
      ICR_IUE | ICR_SCLE | ICR_TB | ICR_STOP | ICR_ACKNAK
      );
    Status = I2cWaitIsr (ISR_IRF);
    *Val   = (UINT8)MmioRead32 (PXA988_TWSI2_BASE + I2C_IDBR);
  }

  if (EFI_ERROR (Status)) {
    I2cStop ();
  }

  return Status;
}
