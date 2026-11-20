/** @file
  ArmPlatformLib for Marvell PXA988 / PXA986.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiPei.h>
#include <Library/ArmLib.h>
#include <Library/ArmPlatformLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/SerialPortLib.h>
#include <Ppi/ArmMpCoreInfo.h>
#include <Pxa988.h>

STATIC ARM_CORE_INFO  mPxa988CoreInfo[] = {
  { 0x0, 0, 0, 0, (UINT64)0xFFFFFFFF },
  { 0x1, 0, 0, 0, (UINT64)0xFFFFFFFF },
};

EFI_BOOT_MODE
ArmPlatformGetBootMode (
  VOID
  )
{
  return BOOT_WITH_FULL_CONFIGURATION;
}

STATIC
VOID
Pxa988L2Disable (
  VOID
  )
{
  //
  // S-Boot may leave the PL310 enabled. EDK2's ARM cache maintenance does
  // not handle an outer cache, so clean+invalidate it and turn it off
  // before the MMU and L1 caches are enabled. Linux enables it again.
  //
  if ((MmioRead32 (PXA988_L2C_BASE + PL310_CTRL) & 1) == 0) {
    return;
  }

  MmioWrite32 (PXA988_L2C_BASE + PL310_CLEAN_INV_WAY, 0xFFFF);
  while ((MmioRead32 (PXA988_L2C_BASE + PL310_CLEAN_INV_WAY) & 0xFFFF) != 0) {
  }

  MmioWrite32 (PXA988_L2C_BASE + PL310_CACHE_SYNC, 0);
  MmioWrite32 (PXA988_L2C_BASE + PL310_CTRL, 0);
  ArmDataSynchronizationBarrier ();
}

//
// Cortex-A9 ACTLR: SMP (take part in coherency) and FW (broadcast cache/TLB maintenance).
// Linux sets both itself before turning the MMU on (__v7_ca9mp_setup); Windows does not and
// maps its memory Shareable, which a Cortex-A9 with SMP=0 treats as non-cacheable and outside
// coherency. Set them here, before caches/MMU, writing only when they are not set already
// (the same write the vendor Linux kernel performs in this CPU mode).
//
#define A9_ACTLR_FW   BIT0
#define A9_ACTLR_SMP  BIT6

STATIC
UINT32
Pxa988EnableSmpMode (
  VOID
  )
{
  UINT32  Actlr;

  Actlr = (UINT32)ArmReadAuxCr ();
  if ((Actlr & (A9_ACTLR_SMP | A9_ACTLR_FW)) != (A9_ACTLR_SMP | A9_ACTLR_FW)) {
    ArmWriteAuxCr (Actlr | A9_ACTLR_SMP | A9_ACTLR_FW);
    ArmInstructionSynchronizationBarrier ();
  }

  return Actlr;
}

RETURN_STATUS
ArmPlatformInitialize (
  IN  UINTN  MpId
  )
{
  UINT32  ActlrBefore;
  UINT32  TraceBefore;

  if ((MpId & 0xFF) != 0) {
    return RETURN_SUCCESS;
  }

  ActlrBefore = Pxa988EnableSmpMode ();
  Pxa988L2Disable ();

  //
  // Clock the CPU debug logic. Windows' kernel programs the CP14 debug registers
  // (OS Lock, DBGDSCRext, breakpoints) at start-up; with the clock gated those
  // instructions are UNDEFINED and Windows stops with 0xC4/0x91.
  //
  TraceBefore = MmioRead32 (APMU_TRACE);
  MmioOr32 (APMU_TRACE, APMU_TRACE_DBGCLK_EN);

  //
  // Peripheral IRQs go straight to the GIC SPIs; the legacy ICU must not
  // also drive the core nIRQ/nFIQ lines (same as Linux pxa988_dt_init_irq)
  //
  MmioWrite32 (ICU_A9C0_GBL_INT_MSK, ICU_GBL_IRQ_FIQ_MSK);
  MmioWrite32 (ICU_A9C1_GBL_INT_MSK, ICU_GBL_IRQ_FIQ_MSK);

  // Start a fresh on-screen log for this boot (see PxaUartSerialPortLib)
  if (FixedPcdGetBool (PcdFbLogEnable)) {
    MmioWrite32 (
      FixedPcdGet32 (PcdFrameBufferBase) +
      ALIGN_VALUE (FixedPcdGet32 (PcdFrameBufferStride) * 4 * FixedPcdGet32 (PcdFrameBufferHeight), 0x1000),
      0
      );
  }

  SerialPortInitialize ();
  DEBUG ((DEBUG_ERROR, "\ngoyawifi UEFI (PXA986) - SEC, built " __DATE__ " " __TIME__ "\n"));
  DEBUG ((DEBUG_ERROR, "ACTLR 0x%08x -> 0x%08x (SMP/FW)\n", ActlrBefore, (UINT32)ArmReadAuxCr ()));
  DEBUG ((DEBUG_ERROR, "APMU_TRACE 0x%08x -> 0x%08x (debug clock)\n", TraceBefore, MmioRead32 (APMU_TRACE)));

  return RETURN_SUCCESS;
}

STATIC
EFI_STATUS
Pxa988GetMpCoreInfo (
  OUT UINTN          *CoreCount,
  OUT ARM_CORE_INFO  **ArmCoreTable
  )
{
  *CoreCount    = ARRAY_SIZE (mPxa988CoreInfo);
  *ArmCoreTable = mPxa988CoreInfo;
  return EFI_SUCCESS;
}

STATIC ARM_MP_CORE_INFO_PPI    mMpCoreInfoPpi = { Pxa988GetMpCoreInfo };
STATIC EFI_PEI_PPI_DESCRIPTOR  mPlatformPpiTable[] = {
  {
    EFI_PEI_PPI_DESCRIPTOR_PPI | EFI_PEI_PPI_DESCRIPTOR_TERMINATE_LIST,
    &gArmMpCoreInfoPpiGuid,
    &mMpCoreInfoPpi
  }
};

VOID
ArmPlatformGetPlatformPpiList (
  OUT UINTN                   *PpiListSize,
  OUT EFI_PEI_PPI_DESCRIPTOR  **PpiList
  )
{
  *PpiListSize = sizeof (mPlatformPpiTable);
  *PpiList     = mPlatformPpiTable;
}
