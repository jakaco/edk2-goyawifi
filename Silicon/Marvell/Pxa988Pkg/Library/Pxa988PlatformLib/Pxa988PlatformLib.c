/** @file
  ArmPlatformLib for Marvell PXA988 / PXA986.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiPei.h>
#include <Library/ArmLib.h>
#include <Library/ArmPlatformLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
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

RETURN_STATUS
ArmPlatformInitialize (
  IN  UINTN  MpId
  )
{
  if ((MpId & 0xFF) != 0) {
    return RETURN_SUCCESS;
  }

  Pxa988L2Disable ();
  SerialPortInitialize ();

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
