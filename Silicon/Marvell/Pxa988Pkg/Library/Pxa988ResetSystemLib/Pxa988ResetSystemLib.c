/** @file
  ResetSystemLib for Marvell PXA988 / PXA986.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/MmpTimerRuntime.h>
#include <Library/Pm822Lib.h>
#include <Library/ResetSystemLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeLib.h>
#include <Pxa988.h>

//
// ResetSystem runs after ExitBootServices, and once the OS has called
// SetVirtualAddressMap() the physical MMIO addresses are gone. Every register
// block this file (and Pm822Lib / MmpTimerLib behind it) touches is therefore
// declared as a runtime MMIO region, and its base is moved to the virtual
// address in the virtual address change notification.
//
#define MPMU_REGION_SIZE  0x2000      // MPMU_CPRR/WDTPCR at +0x20/+0x200, APRR at +0x1020
#define PAGE_REGION_SIZE  0x1000
#define RTC_BASE          0xD4010000

STATIC UINTN      mMpmuBase = PXA988_MPMU_BASE;
STATIC UINTN      mWdtBase  = PXA988_WDT_BASE;
STATIC UINTN      mRtcBase  = RTC_BASE;
STATIC UINTN      mTwsi2    = PXA988_TWSI2_BASE;
STATIC UINTN      mApbcp    = PXA988_APBCP_BASE;
STATIC UINTN      mTimer0   = PXA988_TIMER0_BASE;
STATIC EFI_EVENT  mVirtualAddressChangeEvent;

#define MPMU_REG(Reg)  (mMpmuBase + ((Reg) - PXA988_MPMU_BASE))
#define WDT_REG(Reg)   (mWdtBase + ((Reg) - PXA988_WDT_BASE))

STATIC
VOID
Pxa988WatchdogReset (
  VOID
  )
{
  UINT32  Val;

  // Hold the CP and reset the MSA so they cannot cancel the WDT reset
  MmioOr32 (MPMU_REG (MPMU_APRR), MPMU_APRR_CPR);
  MicroSecondDelay (10);
  MmioOr32 (MPMU_REG (MPMU_CPRR), BIT2 | BIT3);
  MicroSecondDelay (10);

  // Reset and enable the watchdog clock
  MmioWrite32 (MPMU_REG (MPMU_WDTPCR), 0x7);
  MmioRead32 (MPMU_REG (MPMU_WDTPCR));
  MmioWrite32 (MPMU_REG (MPMU_WDTPCR), 0x3);
  MmioRead32 (MPMU_REG (MPMU_WDTPCR));

  MmioWrite32 (WDT_REG (WDT_WFAR), 0xBABA);
  MmioWrite32 (WDT_REG (WDT_WSAR), 0xEB10);
  MmioWrite32 (WDT_REG (WDT_WMER), 0x3);

  MmioOr32 (MPMU_REG (MPMU_APRR), MPMU_APRR_WDTR);

  MmioWrite32 (WDT_REG (WDT_WFAR), 0xBABA);
  MmioWrite32 (WDT_REG (WDT_WSAR), 0xEB10);
  MmioWrite32 (WDT_REG (WDT_WSR), 0);

  Val = MmioRead32 (WDT_REG (WDT_WVR));
  MmioWrite32 (WDT_REG (WDT_WFAR), 0xBABA);
  MmioWrite32 (WDT_REG (WDT_WSAR), 0xEB10);
  MmioWrite32 (WDT_REG (WDT_WMR), (Val + 0x20) & 0xFFFF);

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
    MmioWrite32 (mRtcBase + (PXA988_RTC_BR0 - RTC_BASE), RTC_BR0_RECOVERY);
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

STATIC
VOID
Pxa988DeclareRuntimeMmio (
  IN EFI_PHYSICAL_ADDRESS  Base,
  IN UINT64                Size
  )
{
  EFI_STATUS  Status;

  //
  // The region may already be in the GCD (platform MMIO resource); that is
  // fine, only the attributes matter.
  //
  gDS->AddMemorySpace (EfiGcdMemoryTypeMemoryMappedIo, Base, Size, EFI_MEMORY_UC | EFI_MEMORY_RUNTIME | EFI_MEMORY_XP);
  Status = gDS->SetMemorySpaceAttributes (Base, Size, EFI_MEMORY_UC | EFI_MEMORY_RUNTIME | EFI_MEMORY_XP);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Pxa988ResetSystemLib: runtime MMIO 0x%lx failed: %r\n", Base, Status));
  }
}

STATIC
VOID
EFIAPI
Pxa988VirtualAddressChange (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EfiConvertPointer (0, (VOID **)&mMpmuBase);
  EfiConvertPointer (0, (VOID **)&mWdtBase);
  EfiConvertPointer (0, (VOID **)&mRtcBase);
  EfiConvertPointer (0, (VOID **)&mTwsi2);
  EfiConvertPointer (0, (VOID **)&mApbcp);
  EfiConvertPointer (0, (VOID **)&mTimer0);

  Pm822SetRuntimeBases (mTwsi2, mApbcp);
  MmpTimerSetRuntimeBase (mTimer0);
}

EFI_STATUS
EFIAPI
Pxa988ResetSystemLibConstructor (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  Pxa988DeclareRuntimeMmio (PXA988_MPMU_BASE, MPMU_REGION_SIZE);
  Pxa988DeclareRuntimeMmio (PXA988_WDT_BASE, PAGE_REGION_SIZE);
  Pxa988DeclareRuntimeMmio (RTC_BASE, PAGE_REGION_SIZE);
  Pxa988DeclareRuntimeMmio (PXA988_TWSI2_BASE, PAGE_REGION_SIZE);
  Pxa988DeclareRuntimeMmio (PXA988_APBCP_BASE, PAGE_REGION_SIZE);
  Pxa988DeclareRuntimeMmio (PXA988_TIMER0_BASE, PAGE_REGION_SIZE);

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_NOTIFY,
                  Pxa988VirtualAddressChange,
                  NULL,
                  &gEfiEventVirtualAddressChangeGuid,
                  &mVirtualAddressChangeEvent
                  );
  ASSERT_EFI_ERROR (Status);
  return EFI_SUCCESS;
}
