/** @file
  Move the PXA988/PXA986 application cores to the 1066 MHz operating point (PLL2 / 1) at boot.

  Source of truth is the 4.19 clk-of-pxa988.c "cpu" clock (pxa988_core_fc_sm) and the OPP table of
  the goyawifi DT: 1066 MHz needs buck1 (vcc_main) >= 1.175 V, which is also the DT floor.

  Safety guards (any failure = leave the clock alone and report):
    - PCD PcdDvfsEnable
    - PLL2 must be running and decode to ~1066 MHz
    - the frequency-change engine must be idle and ready
    - buck1 DVC levels below 1.175 V are raised first (never lowered)
    - after the change the core clock select/divider are read back

  The result is kept in the "DvfsStatus" variable (shell: dmpstore DvfsStatus).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/Pm822Lib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Pxa988.h>

#define MPMU_FCCR        (PXA988_MPMU_BASE + 0x0008)
#define MPMU_PLL2CR      (PXA988_MPMU_BASE + 0x0034)
#define APBS_PLL2CR      (0xD4090104UL)

#define APMU_CP_CCR_REG  (PXA988_APMU_BASE + 0x0000)
#define APMU_CCR_REG     (PXA988_APMU_BASE + 0x0004)
#define APMU_CCSR_REG    (PXA988_APMU_BASE + 0x000C)
#define APMU_DEBUG_REG   (PXA988_APMU_BASE + 0x0088)
#define APMU_IMR_REG     (PXA988_APMU_BASE + 0x0098)
#define APMU_ISR_REG     (PXA988_APMU_BASE + 0x00A0)
#define APMU_PLLSEL_REG  (PXA988_APMU_BASE + 0x00C4)

#define PM800_BUCK1_DVC0 0x3C                   // BUCK1..BUCK1_3 = DVC levels 0..3
#define BUCK1_1175MV     0x2E                   // 600 mV + 0x2E * 12.5 mV = 1175 mV

#define TARGET_SEL       2                      // PLL2
#define TARGET_DIV       0x240                  // divider field value from the vendor table: /1

STATIC CHAR8  mReport[200];

STATIC
VOID
Report (
  IN CONST CHAR8 *Text
  )
{
  DEBUG ((DEBUG_INFO, "Dvfs: %a\n", Text));
  AsciiStrCpyS (mReport, sizeof (mReport), Text);
  gRT->SetVariable (
         L"DvfsStatus",
         &gPxa988TokenSpaceGuid,
         EFI_VARIABLE_BOOTSERVICE_ACCESS,
         AsciiStrLen (mReport) + 1,
         mReport
         );
}

STATIC
UINT32
DivSelToDiv (
  IN UINT32 Sel
  )
{
  switch (Sel) {
    case 2: return 2;
    case 4: return 3;
    case 5: return 4;
    case 7: return 6;
    case 8: return 8;
    default: return 1;
  }
}

// PLL2 output in kHz, 0 if it does not look running
STATIC
UINT32
ReadPll2Khz (
  VOID
  )
{
  UINT32 Cr = MmioRead32 (MPMU_PLL2CR);
  UINT32 Refd, Fbd, Apbs;
  UINT64 Vco;

  if (((Cr & BIT9) != 0) && ((Cr & BIT8) == 0)) {
    return 0;
  }

  Refd = (Cr >> 19) & 0x1F;
  if (Refd == 0) {
    Refd = 1;
  }

  Fbd  = (Cr >> 10) & 0x1FF;
  Vco  = (26000ULL * Fbd + Refd - 1) / Refd;
  Apbs = MmioRead32 (APBS_PLL2CR);
  return (UINT32)(Vco / DivSelToDiv ((Apbs >> 4) & 0xF));
}

STATIC
BOOLEAN
WaitIsr (
  IN UINT32 Bit
  )
{
  UINTN I;

  for (I = 0; I < 10000; I++) {
    if ((MmioRead32 (APMU_ISR_REG) & Bit) != 0) {
      return TRUE;
    }

    MicroSecondDelay (1);
  }

  return FALSE;
}

STATIC
VOID
CcsrUnlock (
  VOID
  )
{
  UINT32 V = MmioRead32 (APMU_CCR_REG);

  MmioWrite32 (APMU_CCR_REG, V | BIT31);
  MmioWrite32 (APMU_CCR_REG, V & ~BIT31);
}

// pxa988_core_fc_sm()
STATIC
EFI_STATUS
FrequencyChange (
  IN UINT32 Sel,
  IN UINT32 Div
  )
{
  UINTN  I;
  UINT32 V, Cc;
  BOOLEAN Ok = FALSE;

  for (I = 0; I < 10000; I++) {                       // engine idle (bit24 clear) and ready (bit25 set)
    V = MmioRead32 (APMU_CCSR_REG);
    if (((V & BIT24) == 0) && ((V & BIT25) != 0)) {
      Ok = TRUE;
      break;
    }

    MicroSecondDelay (1);
  }

  if (!Ok) {
    return EFI_NOT_READY;
  }

  MmioOr32 (APMU_IMR_REG, BIT1);
  MmioOr32 (APMU_DEBUG_REG, BIT0 | BIT1 | BIT3 | BIT21 | BIT22);
  MmioOr32 (APMU_CP_CCR_REG, BIT27);
  MmioAnd32 (APMU_ISR_REG, ~(UINT32)BIT1);

  V = MmioRead32 (MPMU_FCCR);
  V = (V & ~(7U << 29)) | (Sel << 29);
  MmioWrite32 (MPMU_FCCR, V);
  (VOID)MmioRead32 (MPMU_FCCR);

  Cc  = MmioRead32 (APMU_CCR_REG);
  Cc &= ~((UINT32)0xFFF | BIT24 | BIT25 | BIT26 | BIT28);
  Cc |= Div | (0x3FU << 18) | BIT27;
  MmioWrite32 (APMU_CCR_REG, Cc | BIT24);

  Ok = WaitIsr (BIT1);
  MmioWrite32 (APMU_CCR_REG, MmioRead32 (APMU_CCR_REG) & ~(UINT32)BIT24);
  MmioAnd32 (APMU_ISR_REG, ~(UINT32)BIT1);

  CcsrUnlock ();
  if (!Ok) {
    return EFI_TIMEOUT;
  }

  if ((((MmioRead32 (APMU_PLLSEL_REG) >> 2) & 3) != Sel) ||
      ((MmioRead32 (APMU_CCSR_REG) & 0xFFF) != Div))
  {
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
DvfsInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  CHAR8      Line[200];
  UINT32     Pll2, CurSel, CurDiv;
  UINT8      Dvc[4];
  UINTN      I;
  BOOLEAN    IrqWasOn;
  EFI_STATUS Status;

  if (!FixedPcdGetBool (PcdDvfsEnable)) {
    return EFI_SUCCESS;
  }

  CurSel = (MmioRead32 (APMU_PLLSEL_REG) >> 2) & 3;
  CurDiv = MmioRead32 (APMU_CCSR_REG) & 0xFFF;
  Pll2   = ReadPll2Khz ();
  DEBUG ((DEBUG_INFO, "Dvfs: core select=%u divider=0x%03x PLL2=%u kHz\n", CurSel, CurDiv, Pll2));

  if ((CurSel == TARGET_SEL) && (CurDiv == TARGET_DIV)) {
    AsciiSPrint (Line, sizeof (Line), "already at PLL2/1 (%u kHz)", Pll2);
    Report (Line);
    return EFI_SUCCESS;
  }

  if ((Pll2 < 1060000) || (Pll2 > 1072000)) {
    AsciiSPrint (Line, sizeof (Line), "skipped: PLL2 is %u kHz, not ~1066000 (select=%u div=0x%03x)", Pll2, CurSel, CurDiv);
    Report (Line);
    return EFI_SUCCESS;
  }

  // buck1 voltage floor: raise any DVC level below 1.175 V (never lower one)
  for (I = 0; I < 4; I++) {
    Status = Pm822Read (PM822_POWER_I2C_ADDR, (UINT8)(PM800_BUCK1_DVC0 + I), &Dvc[I]);
    if (EFI_ERROR (Status)) {
      AsciiSPrint (Line, sizeof (Line), "skipped: cannot read buck1 DVC%u (%r)", I, Status);
      Report (Line);
      return EFI_SUCCESS;
    }
  }

  for (I = 0; I < 4; I++) {
    if ((Dvc[I] & 0x7F) < BUCK1_1175MV) {
      Pm822Write (PM822_POWER_I2C_ADDR, (UINT8)(PM800_BUCK1_DVC0 + I), (UINT8)((Dvc[I] & 0x80) | BUCK1_1175MV));
    }
  }

  MicroSecondDelay (500);                               // ramp

  IrqWasOn = ArmGetInterruptState ();
  ArmDisableInterrupts ();
  Status = FrequencyChange (TARGET_SEL, TARGET_DIV);
  if (IrqWasOn) {
    ArmEnableInterrupts ();
  }

  AsciiSPrint (
    Line,
    sizeof (Line),
    "%a (%r): buck1 DVC=%02x/%02x/%02x/%02x, now select=%u div=0x%03x",
    EFI_ERROR (Status) ? "frequency change FAILED" : "CPU at 1066 MHz",
    Status,
    Dvc[0], Dvc[1], Dvc[2], Dvc[3],
    (MmioRead32 (APMU_PLLSEL_REG) >> 2) & 3,
    MmioRead32 (APMU_CCSR_REG) & 0xFFF
    );
  Report (Line);
  return EFI_SUCCESS;
}
