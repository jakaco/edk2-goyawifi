/** @file
  Summit SMB328 charger bring-up for the Galaxy Tab 3 Lite.

  The charger sits on a bit-banged I2C bus (GPIO16 = SCL, GPIO17 = SDA, 7-bit address 0x34) with an
  enable pin on GPIO98 (active low in the vendor DT) and an interrupt on GPIO8 (not used here).
  Register values and the configure/enable sequence follow the 4.19 smb328-charger.c: AC mode input
  limit 500 mA, fast charge 500 mA, float 4.32 V, 200 mA termination.

  There is no cable detection (the RT8973 MUIC is not read) and no battery temperature/voltage
  reading: charging is enabled unconditionally and the charger's own hardware protections apply.
  When no cable is present enabling is harmless. The SMB328 watchdog stops charging if nobody talks
  to it, so charging runs while UEFI runs; Windows needs its own charger driver.

  The last result is kept in the "ChargerStatus" variable (shell: dmpstore ChargerStatus).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Pxa988.h>

#define PIN_SCL    16
#define PIN_SDA    17
#define PIN_EN     98
#define MFPR_CHG_SCL  0x011C
#define MFPR_CHG_SDA  0x0120
#define MFPR_CHG_EN   0x0234

#define GPIO_GPDR_OFF  0x0C
#define GPIO_GPSR_OFF  0x18
#define GPIO_GPCR_OFF  0x24

#define SMB_ADDR  0x34

#define SMB_CHARGE_CURRENT  0x00
#define SMB_INPUT_LIMIT     0x01
#define SMB_FLOAT_VOLTAGE   0x02
#define SMB_FUNC_CTRL_A     0x03
#define SMB_FUNC_CTRL_B     0x04
#define SMB_FUNC_CTRL_C     0x05
#define SMB_OTG_LDO_CTRL    0x06
#define SMB_VARIOUS_FUNCS   0x07
#define SMB_IRQ_CLEAR       0x30
#define SMB_COMMAND         0x31
#define SMB_STATUS_C        0x36
#define SMB_IRQ_STATUS_C    0x37

#define CMD_ALLOW_WRITE     BIT7
#define CMD_CHARGE_DISABLE  BIT4
#define CMD_AC_MODE         BIT3
#define CMD_USB_MODE        BIT2

#define IRQ_C_WATCHDOG      BIT7
#define IRQ_C_FAULT_MASK    (BIT1 | BIT2)

#define FLOAT_UV            4320000U

STATIC EFI_EVENT  mTimer;
STATIC BOOLEAN    mConfigured;
STATIC UINT8      mLastStat = 0xFF;
STATIC CHAR8      mReport[160];

//
// ---- GPIO bit-bang I2C (open drain: low = drive 0, high = release to the pull-up) ----
//
STATIC
VOID
Gpio (
  IN UINTN   Pin,
  IN UINTN   RegOff,
  IN BOOLEAN Set
  )
{
  UINTN Reg = GPIO_BANK (Pin) + RegOff;

  if (Set) {
    MmioOr32 (Reg, 1U << (Pin % 32));
  } else {
    MmioAnd32 (Reg, ~(1U << (Pin % 32)));
  }
}

STATIC VOID SclLow (VOID)  { Gpio (PIN_SCL, GPIO_GPCR_OFF, TRUE); Gpio (PIN_SCL, GPIO_GPDR_OFF, TRUE); }
STATIC VOID SclHigh (VOID) { Gpio (PIN_SCL, GPIO_GPDR_OFF, FALSE); }
STATIC VOID SdaLow (VOID)  { Gpio (PIN_SDA, GPIO_GPCR_OFF, TRUE); Gpio (PIN_SDA, GPIO_GPDR_OFF, TRUE); }
STATIC VOID SdaHigh (VOID) { Gpio (PIN_SDA, GPIO_GPDR_OFF, FALSE); }

STATIC
BOOLEAN
SdaRead (
  VOID
  )
{
  return (MmioRead32 (GPIO_BANK (PIN_SDA) + GPIO_GPLR) & (1U << (PIN_SDA % 32))) != 0;
}

#define HALF_US  5

STATIC
VOID
I2cStart (
  VOID
  )
{
  SdaHigh ();
  SclHigh ();
  MicroSecondDelay (HALF_US);
  SdaLow ();
  MicroSecondDelay (HALF_US);
  SclLow ();
}

STATIC
VOID
I2cStop (
  VOID
  )
{
  SdaLow ();
  MicroSecondDelay (HALF_US);
  SclHigh ();
  MicroSecondDelay (HALF_US);
  SdaHigh ();
  MicroSecondDelay (HALF_US);
}

// returns TRUE if the slave ACKed
STATIC
BOOLEAN
I2cWriteByte (
  IN UINT8 Byte
  )
{
  UINTN   Bit;
  BOOLEAN Ack;

  for (Bit = 0; Bit < 8; Bit++) {
    if (Byte & 0x80) {
      SdaHigh ();
    } else {
      SdaLow ();
    }
    Byte <<= 1;
    MicroSecondDelay (HALF_US);
    SclHigh ();
    MicroSecondDelay (HALF_US);
    SclLow ();
  }

  SdaHigh ();
  MicroSecondDelay (HALF_US);
  SclHigh ();
  MicroSecondDelay (HALF_US);
  Ack = !SdaRead ();
  SclLow ();
  return Ack;
}

STATIC
UINT8
I2cReadByte (
  IN BOOLEAN Nak
  )
{
  UINTN Bit;
  UINT8 Byte = 0;

  SdaHigh ();
  for (Bit = 0; Bit < 8; Bit++) {
    MicroSecondDelay (HALF_US);
    SclHigh ();
    MicroSecondDelay (HALF_US);
    Byte = (UINT8)((Byte << 1) | (SdaRead () ? 1 : 0));
    SclLow ();
  }

  if (Nak) {
    SdaHigh ();
  } else {
    SdaLow ();
  }
  MicroSecondDelay (HALF_US);
  SclHigh ();
  MicroSecondDelay (HALF_US);
  SclLow ();
  SdaHigh ();
  return Byte;
}

STATIC
EFI_STATUS
SmbWriteRaw (
  IN UINT8 Reg,
  IN UINT8 Val
  )
{
  EFI_STATUS Status = EFI_DEVICE_ERROR;

  I2cStart ();
  if (I2cWriteByte (SMB_ADDR << 1) && I2cWriteByte (Reg) && I2cWriteByte (Val)) {
    Status = EFI_SUCCESS;
  }

  I2cStop ();
  return Status;
}

STATIC
EFI_STATUS
SmbRead (
  IN  UINT8 Reg,
  OUT UINT8 *Val
  )
{
  EFI_STATUS Status = EFI_DEVICE_ERROR;

  I2cStart ();
  if (I2cWriteByte (SMB_ADDR << 1) && I2cWriteByte (Reg)) {
    I2cStart ();                                    // repeated start (SMBus read byte)
    if (I2cWriteByte ((SMB_ADDR << 1) | 1)) {
      *Val   = I2cReadByte (TRUE);
      Status = EFI_SUCCESS;
    }
  }

  I2cStop ();
  return Status;
}

// write + verify, like smb328_write()
STATIC
EFI_STATUS
SmbWrite (
  IN UINT8 Reg,
  IN UINT8 Val
  )
{
  UINT8      Rb;
  EFI_STATUS Status;

  Status = SmbWriteRaw (Reg, Val);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SmbRead (Reg, &Rb);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return (Rb == Val) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

#define TRY(x)  do { Status = (x); if (EFI_ERROR (Status)) { Step = __LINE__; goto Close; } } while (0)

// smb328_configure(dedicated = false) + smb328_set_enable(true)
STATIC
EFI_STATUS
SmbConfigureAndEnable (
  VOID
  )
{
  EFI_STATUS Status, CloseStatus;
  UINT8      Cmd, V, FloatCode, Fast = 0, In = 0;
  UINTN      Step = 0;

  FloatCode = (UINT8)((FLOAT_UV / 1000 - 3460) / 20);

  Status = SmbRead (SMB_COMMAND, &Cmd);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Cmd = (UINT8)((Cmd | CMD_ALLOW_WRITE | CMD_CHARGE_DISABLE) & ~(CMD_AC_MODE | CMD_USB_MODE));
  Status = SmbWrite (SMB_COMMAND, Cmd);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  TRY (SmbWrite (SMB_FUNC_CTRL_C, 0x00));
  TRY (SmbWrite (SMB_FUNC_CTRL_B, 0x6E));
  TRY (SmbWrite (SMB_FUNC_CTRL_A, 0xDA));

  TRY (SmbRead (SMB_FLOAT_VOLTAGE, &V));
  TRY (SmbWrite (SMB_FLOAT_VOLTAGE, (UINT8)((V & ~0x7E) | (FloatCode << 1))));

  TRY (SmbRead (SMB_CHARGE_CURRENT, &V));                      // 500 mA fast charge, 200 mA termination
  TRY (SmbWrite (SMB_CHARGE_CURRENT, (UINT8)((V & 0x18) | (Fast << 5) | 0x07)));

  TRY (SmbRead (SMB_INPUT_LIMIT, &V));                         // 500 mA input limit
  TRY (SmbWrite (SMB_INPUT_LIMIT, (UINT8)((V & 0x18) | (In << 5))));

  TRY (SmbRead (SMB_VARIOUS_FUNCS, &V));
  TRY (SmbWrite (SMB_VARIOUS_FUNCS, (UINT8)((V & 0x1F) | 0xE0)));

  TRY (SmbRead (SMB_OTG_LDO_CTRL, &V));                        // never source VBUS
  TRY (SmbWrite (SMB_OTG_LDO_CTRL, (UINT8)(V & ~BIT5)));

  Cmd |= CMD_AC_MODE;

Close:
  CloseStatus = SmbWrite (SMB_COMMAND, (UINT8)(Cmd & ~CMD_ALLOW_WRITE));
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Charger: configure failed at line %u: %r\n", Step, Status));
    return Status;
  }

  if (EFI_ERROR (CloseStatus)) {
    return CloseStatus;
  }

  // enable charging
  Cmd = (UINT8)(Cmd & ~(CMD_ALLOW_WRITE | CMD_CHARGE_DISABLE));
  return SmbWrite (SMB_COMMAND, Cmd);
}

STATIC
VOID
Report (
  IN CONST CHAR8 *Text
  )
{
  AsciiSPrint (mReport, sizeof (mReport), "%a", Text);
  gRT->SetVariable (
         L"ChargerStatus",
         &gPxa988TokenSpaceGuid,
         EFI_VARIABLE_BOOTSERVICE_ACCESS,
         AsciiStrLen (mReport) + 1,
         mReport
         );
}

STATIC
VOID
EFIAPI
OnTimer (
  IN EFI_EVENT Event,
  IN VOID      *Context
  )
{
  UINT8      Stat = 0, Irq = 0;
  EFI_STATUS Status;
  CHAR8      Line[160];

  if (!mConfigured) {
    Status = SmbConfigureAndEnable ();
    if (EFI_ERROR (Status)) {
      Report ("SMB328 configure failed (no ACK or readback mismatch)");
      return;
    }

    mConfigured = TRUE;
    DEBUG ((DEBUG_INFO, "Charger: SMB328 configured, charging enabled\n"));
  }

  Status = SmbRead (SMB_IRQ_STATUS_C, &Irq);
  if (!EFI_ERROR (Status) && ((Irq & IRQ_C_WATCHDOG) != 0)) {
    // the charger watchdog fired: clear it and program the charger again (as the vendor driver does)
    SmbWriteRaw (SMB_IRQ_CLEAR, 0xFF);
    mConfigured = FALSE;
    return;
  }

  Status = SmbRead (SMB_STATUS_C, &Stat);
  if (EFI_ERROR (Status)) {
    mConfigured = FALSE;
    Report ("SMB328 status read failed");
    return;
  }

  if (Stat != mLastStat) {
    mLastStat = Stat;
    AsciiSPrint (
      Line,
      sizeof (Line),
      "STATUS_C=0x%02x IRQ_C=0x%02x enabled=%d charging-bits=%d error-bits=%d termination=%d",
      Stat,
      Irq,
      Stat & 1,
      (Stat >> 1) & 3,
      (Stat >> 4) & 3,
      (Stat >> 6) & 3
      );
    DEBUG ((DEBUG_INFO, "Charger: %a\n", Line));
    Report (Line);
  }
}

EFI_STATUS
EFIAPI
ChargerInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS Status;

  // GPIO block clock (normally already on)
  MmioOr32 (APBC_GPIO, APBC_APBCLK | APBC_FNCLK);

  // pins as GPIO (AF0): SCL/SDA with pull-ups, EN without
  MmioAndThenOr32 (PXA988_MFPR_BASE + MFPR_CHG_SCL, ~(UINT32)(MFPR_PULL_MASK | MFPR_AF_MASK), MFPR_PULL_UP);
  MmioAndThenOr32 (PXA988_MFPR_BASE + MFPR_CHG_SDA, ~(UINT32)(MFPR_PULL_MASK | MFPR_AF_MASK), MFPR_PULL_UP);
  MmioAnd32 (PXA988_MFPR_BASE + MFPR_CHG_EN, ~(UINT32)(MFPR_PULL_MASK | MFPR_AF_MASK));

  SclHigh ();
  SdaHigh ();

  // CHG_EN is "active low" in the vendor DT (enable-gpios ... GPIO_ACTIVE_LOW): drive it low to enable.
  // (The vendor driver itself never asserts it; if charging does not start, try driving it high.)
  Gpio (PIN_EN, GPIO_GPCR_OFF, TRUE);
  Gpio (PIN_EN, GPIO_GPDR_OFF, TRUE);

  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, OnTimer, NULL, &mTimer);
  if (!EFI_ERROR (Status)) {
    Status = gBS->SetTimer (mTimer, TimerPeriodic, 2 * 10000000ULL);       // every 2 s
  }

  return Status;
}
