/** @file
  Second core (CPU1) bring-up for the PXA988 / PXA986.

  Release sequence taken from the vendor kernels (4.19 platsmp-pxa988.c, 3.4
  reset-pxa988.c): the boot ROM starts CPU1 at the physical address in
  CIU_WARM_RESET_VECTOR once its reset bits in APMU_CC2_AP are cleared.

  This driver only proves that the core runs and answers (the stub in
  Cpu1Stub.S, MMU and caches off) and then puts it back into reset before
  ExitBootServices, because the OS kernel releases CPU1 itself.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/ArmLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Pxa988.h>

#define MB_OFFSET       0x100
#define MB_ALIVE        0x00
#define MB_MIDR         0x04
#define MB_MPIDR        0x08
#define MB_SCTLR        0x0C
#define MB_HEARTBEAT    0x10
#define MB_CMD          0x40
#define MB_ACK          0x18
#define MB_ARG          0x44
#define MB_RESULT       0x20
#define MB_STATE        0x24
#define ALIVE_MAGIC     0xA9A9C0DE

extern CONST UINT8  Pxa988Cpu1StubStart[];
extern CONST UINT8  Pxa988Cpu1StubEnd[];

STATIC UINT8      *mPage;

STATIC
VOID
SendEvent (
  VOID
  )
{
  __asm__ volatile ("dsb\n\tsev" ::: "memory");
}

STATIC UINT32     mSavedVector;
STATIC UINT32     mSavedCc2;
STATIC BOOLEAN    mCpu1Released;
STATIC EFI_EVENT  mExitBsEvent;
STATIC EFI_EVENT  mEndOfDxeEvent;
STATIC CHAR8      mReport[512];
STATIC UINTN      mReportLen;

/**
  Print a line to the debug log and keep it for the "Cpu1Status" variable
  (shown by the UEFI Shell with: dmpstore Cpu1Status).
**/
STATIC
VOID
Log (
  IN CONST CHAR8  *Format,
  ...
  )
{
  VA_LIST  Args;
  CHAR8    Line[160];
  UINTN    Len;

  VA_START (Args, Format);
  Len = AsciiVSPrint (Line, sizeof (Line), Format, Args);
  VA_END (Args);
  DEBUG ((DEBUG_INFO, "Pxa988Smp: %a", Line));
  if (mReportLen + Len < sizeof (mReport)) {
    CopyMem (mReport + mReportLen, Line, Len);
    mReportLen += Len;
  }
}

STATIC
VOID
EFIAPI
PublishReport (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  gBS->CloseEvent (Event);
  gRT->SetVariable (
         L"Cpu1Status",
         &gPxa988TokenSpaceGuid,
         EFI_VARIABLE_BOOTSERVICE_ACCESS,
         mReportLen,
         mReport
         );
}


STATIC
UINT32
MbRead (
  IN UINTN  Offset
  )
{
  //
  // CPU1 writes with caches off; drop whatever CPU0 has cached of the line
  //
  InvalidateDataCacheRange (mPage + MB_OFFSET, 0x40);        // CPU1's lines only
  return *(volatile UINT32 *)(mPage + MB_OFFSET + Offset);
}

STATIC
VOID
MbWrite (
  IN UINTN   Offset,
  IN UINT32  Value
  )
{
  *(volatile UINT32 *)(mPage + MB_OFFSET + Offset) = Value;
  WriteBackDataCacheRange (mPage + MB_OFFSET + 0x40, 0x40);   // CPU0's own lines only
  ArmDataSynchronizationBarrier ();
}

STATIC
BOOLEAN
MbWaitFor (
  IN UINTN   Offset,
  IN UINT32  Value,
  IN UINTN   TimeoutMs
  )
{
  while (TimeoutMs-- > 0) {
    if (MbRead (Offset) == Value) {
      return TRUE;
    }

    MicroSecondDelay (1000);
  }

  return MbRead (Offset) == Value;
}

STATIC
VOID
Cpu1Hold (
  VOID
  )
{
  MmioWrite32 (APMU_CC2_AP, MmioRead32 (APMU_CC2_AP) | CC2_CPU1_RST | CC2_CPU1_DBG_RST | CC2_CPU1_WDOG_RST);
  MmioRead32 (APMU_CC2_AP);
}

STATIC
VOID
Cpu1Release (
  VOID
  )
{
  UINT32  Cc2;

  Cc2 = MmioRead32 (APMU_CC2_AP);
  if ((Cc2 & CC2_CPU1_RST) == 0) {
    //
    // The previous bootloader left CPU1 running: reset it first
    //
    MmioWrite32 (APMU_CC2_AP, Cc2 | CC2_CPU1_RST);
    MmioRead32 (APMU_CC2_AP);
    MicroSecondDelay (10);
    Cc2 = MmioRead32 (APMU_CC2_AP);
  }

  MmioWrite32 (APMU_CC2_AP, Cc2 & ~(CC2_CPU1_RST | CC2_CPU1_DBG_RST | CC2_CPU1_WDOG_RST));
  MmioRead32 (APMU_CC2_AP);
  MmioWrite32 (APMU_CORE_WAKEUP_CTL, APMU_WAKEUP_CORE1);
  ArmDataSynchronizationBarrier ();
}

/**
  Park CPU1 in reset again and restore what the boot ROM vector held.
**/
STATIC
VOID
EFIAPI
Cpu1Shutdown (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  if (!mCpu1Released) {
    return;
  }

  MbWrite (MB_CMD, 1);                    // ask it to park
  SendEvent ();
  MbWaitFor (MB_STATE, 2, 20);

  MmioWrite32 (APMU_CC2_AP, (MmioRead32 (APMU_CC2_AP) & ~(CC2_CPU1_RST | CC2_CPU1_DBG_RST | CC2_CPU1_WDOG_RST)) |
                            (mSavedCc2 & (CC2_CPU1_RST | CC2_CPU1_DBG_RST | CC2_CPU1_WDOG_RST)));
  Cpu1Hold ();                            // OS expects CPU1 held in reset
  MmioWrite32 (CIU_WARM_RESET_VECTOR, mSavedVector);
  mCpu1Released = FALSE;
  Log ("CPU1 parked\n");
}

EFI_STATUS
EFIAPI
Pxa988SmpInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS            Status;
  EFI_PHYSICAL_ADDRESS  Page;
  UINTN                 StubSize;
  UINT32                Beat0;
  UINT32                Beat1;

  gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, PublishReport, NULL, &gEfiEndOfDxeEventGroupGuid, &mEndOfDxeEvent);

  // What the OS will inherit (set in Pxa988PlatformLib before the MMU is enabled)
  Log ("ACTLR=0x%08x (SMP %a, FW %a)\n",
    (UINT32)ArmReadAuxCr (),
    (ArmReadAuxCr () & BIT6) ? "on" : "OFF",
    (ArmReadAuxCr () & BIT0) ? "on" : "OFF");

  if (!FixedPcdGetBool (PcdSecondaryCpuBringup)) {
    return EFI_SUCCESS;
  }

  StubSize = (UINTN)(Pxa988Cpu1StubEnd - Pxa988Cpu1StubStart);
  ASSERT (StubSize == MB_OFFSET + 0x80);

  Page   = 0;
  Status = gBS->AllocatePages (AllocateAnyPages, EfiBootServicesData, 1, &Page);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mPage = (UINT8 *)(UINTN)Page;
  ZeroMem (mPage, EFI_PAGE_SIZE);
  CopyMem (mPage, Pxa988Cpu1StubStart, StubSize);
  ZeroMem (mPage + MB_OFFSET, 0x80);
  WriteBackInvalidateDataCacheRange (mPage, EFI_PAGE_SIZE);
  ArmInvalidateInstructionCache ();
  ArmDataSynchronizationBarrier ();

  mSavedVector = MmioRead32 (CIU_WARM_RESET_VECTOR);
  mSavedCc2    = MmioRead32 (APMU_CC2_AP);
  Log ("CC2=0x%08x warm-reset vector=0x%08x, stub at 0x%08x\n",
    mSavedCc2,
    mSavedVector,
    (UINT32)Page);

  MmioWrite32 (CIU_WARM_RESET_VECTOR, (UINT32)Page);
  MmioRead32 (CIU_WARM_RESET_VECTOR);
  mCpu1Released = TRUE;
  Cpu1Release ();

  if (!MbWaitFor (MB_ALIVE, ALIVE_MAGIC, 500)) {
    Log ("CPU1 did not come up (CC2=0x%08x)\n", MmioRead32 (APMU_CC2_AP));
    Cpu1Hold ();
    MmioWrite32 (CIU_WARM_RESET_VECTOR, mSavedVector);
    mCpu1Released = FALSE;
    gBS->FreePages (Page, 1);
    return EFI_SUCCESS;
  }

  Log ("CPU1 is up: MIDR=0x%08x MPIDR=0x%08x SCTLR=0x%08x\n",
    MbRead (MB_MIDR),
    MbRead (MB_MPIDR),
    MbRead (MB_SCTLR));

  //
  // Round trip: write an argument, expect the bitwise complement back
  //
  MbWrite (MB_ARG, 0x12345678);
  MbWrite (MB_CMD, 2);
  SendEvent ();
  if (MbWaitFor (MB_ACK, 2, 100) && (MbRead (MB_RESULT) == ~0x12345678U)) {
    Log ("CPU1 ping OK\n");
  } else {
    Log ("CPU1 ping failed (ack=%u result=0x%08x)\n", MbRead (MB_ACK), MbRead (MB_RESULT));
  }

  Beat0 = MbRead (MB_HEARTBEAT);
  SendEvent ();
  MicroSecondDelay (1000);
  Beat1 = MbRead (MB_HEARTBEAT);
  Log ("CPU1 heartbeat %u -> %u\n", Beat0, Beat1);

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  Cpu1Shutdown,
                  NULL,
                  &gEfiEventExitBootServicesGuid,
                  &mExitBsEvent
                  );
  ASSERT_EFI_ERROR (Status);

  return EFI_SUCCESS;
}
