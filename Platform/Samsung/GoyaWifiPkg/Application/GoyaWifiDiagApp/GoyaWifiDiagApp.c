/*
 * GoyaWifiDiag: what the firmware sees on the Galaxy Tab 3 Lite 7.0 Wi-Fi
 * (goyawifi, SM-T110, Marvell PXA986).
 *
 * Read-only apart from the colour test, which paints the screen, and safe to
 * start from the boot manager menu. Every register block is only touched when
 * its clock is known to be running, because reading a gated PXA block hangs
 * the bus.
 *
 * Keys:  Home = Enter = next / action     Power = Esc = quit
 *
 *   Page 1  firmware, CPU, timer interrupt test, GIC, memory
 *   Page 2  PMIC, display, SD controllers, disks
 *   Page 3  second core and USB host (results of the bring-up drivers)
 *   Page 4  colour test
 *   Page 5  live key test
 *
 * Inspired by RossaDiag (notnoelchannel2/rossadiag) for the Galaxy Core Prime.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#include <Uefi.h>
#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/Pm822Lib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Protocol/BlockIo.h>
#include <Protocol/Cpu.h>
#include <Protocol/GraphicsOutput.h>
#include <Pxa988.h>

#define GICD_CTLR    0x000
#define GICD_TYPER   0x004
#define GICD_IIDR    0x008
#define GICC_CTLR    0x000
#define GICC_IIDR    0x0FC
#define PXA_GIC_D    0xD1DFF000
#define PXA_GIC_C    0xD1DFE100

#define SDHCI_PRESENT_STATE  0x24
#define SDHCI_HOST_CTRL1     0x28
#define SDHCI_CLOCK_CTRL     0x2C
#define SDHCI_CAPS0          0x40
#define SDHCI_CAPS1          0x44
#define SDHCI_HOST_CTRL2     0x3E

#define KEY_COUNT  4

//
// Raw key levels: Vol Up GPIO1, Vol Down GPIO2, Home GPIO13 (active low),
// Power = PMIC ONKEY (active high)
//
typedef struct {
  CONST CHAR16    *Name;
  UINT8           Gpio;      // 0xFF = PMIC onkey
} RAW_KEY;

STATIC CONST RAW_KEY  mRawKeys[KEY_COUNT] = {
  { L"Vol+",  1    },
  { L"Vol-",  2    },
  { L"Home",  13   },
  { L"Power", 0xFF },
};

STATIC
BOOLEAN
RawKeyDown (
  IN CONST RAW_KEY  *Key
  )
{
  UINT8  Val;

  if (Key->Gpio == 0xFF) {
    return !EFI_ERROR (Pm822Read (PM822_I2C_ADDR, PM800_STATUS_1, &Val)) && ((Val & PM800_ONKEY_STS) != 0);
  }

  return (MmioRead32 (GPIO_BANK (Key->Gpio) + GPIO_GPLR) & (1U << (Key->Gpio % 32))) == 0;
}

/**
  Wait for a key; Esc (Power) quits. Returns TRUE to continue.
**/
STATIC
BOOLEAN
NextPage (
  IN CONST CHAR16  *Action
  )
{
  EFI_INPUT_KEY  Key;
  UINTN          Index;

  Print (L"\n[Home] %s    [Power] quit\n", Action);
  gST->ConIn->Reset (gST->ConIn, FALSE);
  for ( ; ; ) {
    gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &Index);
    if (EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key))) {
      continue;
    }

    if (Key.ScanCode == SCAN_ESC) {
      return FALSE;
    }

    if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
      return TRUE;
    }
  }
}

/**
  Print a block of ASCII text a line at a time (the console wants CR+LF).
**/
STATIC
VOID
PrintLines (
  IN CONST CHAR8  *Text,
  IN UINTN        Size
  )
{
  UINTN  Start = 0;
  UINTN  I;

  for (I = 0; I <= Size; I++) {
    if ((I == Size) || (Text[I] == '\n')) {
      if (I > Start) {
        Print (L"  %.*a\n", I - Start, Text + Start);
      }

      Start = I + 1;
    }
  }
}

STATIC
VOID
ShowVariable (
  IN CONST CHAR16  *Name,
  IN CONST CHAR16  *Missing
  )
{
  UINTN   Size = 0;
  CHAR8   *Buf;
  EFI_STATUS  Status;

  Status = gRT->GetVariable ((CHAR16 *)Name, &gPxa988TokenSpaceGuid, NULL, &Size, NULL);
  if ((Status != EFI_BUFFER_TOO_SMALL) || (Size == 0)) {
    Print (L"  %s\n", Missing);
    return;
  }

  Buf = AllocatePool (Size);
  if ((Buf != NULL) && !EFI_ERROR (gRT->GetVariable ((CHAR16 *)Name, &gPxa988TokenSpaceGuid, NULL, &Size, Buf))) {
    PrintLines (Buf, Size);
  }

  FreePool (Buf);
}

STATIC UINTN  mTicks;

STATIC
VOID
EFIAPI
TickNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  mTicks++;
}

/**
  Count 100 ms timer events during one second of busy waiting: proves that the
  timer interrupt reaches the GIC and the CPU.
**/
STATIC
VOID
ShowTimerIrq (
  VOID
  )
{
  EFI_EVENT  Event = NULL;

  mTicks = 0;
  if (EFI_ERROR (gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, TickNotify, NULL, &Event))) {
    Print (L"Timer IRQ test: cannot create event\n");
    return;
  }

  gBS->SetTimer (Event, TimerPeriodic, 1000000);      // 100 ms
  gBS->Stall (1000000);
  gBS->SetTimer (Event, TimerCancel, 0);
  gBS->CloseEvent (Event);
  Print (L"Timer IRQ: %u events in 1 s (expect 10) %s\n", mTicks, (mTicks >= 9 && mTicks <= 11) ? L"OK" : L"CHECK");
}

//
// CPU debug registers (CP14). Windows' kernel programs them while it starts
// (OS Lock, DBGPRSR, DBGDSCRext, DBGVCR, breakpoints). If the debug logic is
// powered down or locked, those instructions are UNDEFINED and Windows stops
// very early. Each probe is one instruction (DbgProbe.S); an Undefined
// Instruction / Data Abort exception is caught here and reported.
//
typedef UINT32 (EFIAPI *DBG_PROBE)(UINT32 Value);
UINT32 EFIAPI GoyaDbgIdDfr0 (UINT32);
UINT32 EFIAPI GoyaDbgDidr (UINT32);
UINT32 EFIAPI GoyaDbgDrar (UINT32);
UINT32 EFIAPI GoyaDbgDsar (UINT32);
UINT32 EFIAPI GoyaDbgOslsr (UINT32);
UINT32 EFIAPI GoyaDbgPrsr (UINT32);
UINT32 EFIAPI GoyaDbgOslarW (UINT32);
UINT32 EFIAPI GoyaDbgDscrInt (UINT32);
UINT32 EFIAPI GoyaDbgDscrExt (UINT32);
UINT32 EFIAPI GoyaDbgDscrExtW (UINT32);
UINT32 EFIAPI GoyaDbgVcrW (UINT32);
UINT32 EFIAPI GoyaDbgBcr0 (UINT32);
UINT32 EFIAPI GoyaDbgBcr0W (UINT32);
UINT32 EFIAPI GoyaDbgBvr0W (UINT32);
UINT32 EFIAPI GoyaDbgWcr0W (UINT32);
UINT32 EFIAPI GoyaDbgWfar (UINT32);

STATIC volatile UINTN  mDbgFault;

STATIC
VOID
EFIAPI
DbgProbeFaultHandler (
  IN EFI_EXCEPTION_TYPE  ExceptionType,
  IN EFI_SYSTEM_CONTEXT  SystemContext
  )
{
  EFI_SYSTEM_CONTEXT_ARM  *Ctx = SystemContext.SystemContextArm;

  mDbgFault = ExceptionType;
  Ctx->R0   = 0xDEADC0DE;
  // Return to the probe's caller; clear the IT bits and set the Thumb bit from lr
  Ctx->CPSR = (Ctx->CPSR & ~(UINT32)(0x20 | 0x0600FC00)) | ((Ctx->LR & 1) ? 0x20 : 0);
  Ctx->PC   = Ctx->LR & ~(UINT32)1;
}

STATIC
BOOLEAN
DbgProbe (
  IN  DBG_PROBE  Probe,
  IN  UINT32     In,
  OUT UINT32     *Out
  )
{
  mDbgFault = 0;
  *Out      = Probe (In);
  return mDbgFault == 0;
}

STATIC
VOID
DbgShow (
  IN CONST CHAR16  *Name,
  IN DBG_PROBE     Probe,
  IN UINT32        In,
  IN BOOLEAN       Write,
  OUT UINT32       *Out
  )
{
  UINT32  V = 0;

  if (DbgProbe (Probe, In, &V)) {
    if (Write) {
      Print (L" %s<-%x ok", Name, In);
    } else {
      Print (L" %s=%08x", Name, V);
    }
  } else {
    Print (L" %s=%s", Name, (mDbgFault == EXCEPT_ARM_UNDEFINED_INSTRUCTION) ? L"UNDEF" : L"ABORT");
    V = 0;
  }

  if (Out != NULL) {
    *Out = V;
  }
}

STATIC
VOID
ShowCpuDebug (
  VOID
  )
{
  EFI_CPU_ARCH_PROTOCOL  *Cpu;
  UINT32                 V;
  UINT32                 Oslsr;
  UINT32                 Prsr;
  UINT32                 Dscr;
  BOOLEAN                Undef, Abort;

  if (EFI_ERROR (gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&Cpu))) {
    Print (L"Debug regs: no CPU protocol\n");
    return;
  }

  Undef = !EFI_ERROR (Cpu->RegisterInterruptHandler (Cpu, EXCEPT_ARM_UNDEFINED_INSTRUCTION, DbgProbeFaultHandler));
  Abort = !EFI_ERROR (Cpu->RegisterInterruptHandler (Cpu, EXCEPT_ARM_DATA_ABORT, DbgProbeFaultHandler));
  if (!Undef) {
    Print (L"Debug regs: cannot hook Undefined Instruction, skipped\n");
    goto Done;
  }

  Print (L"Debug (what Windows does at start-up):\n ");
  DbgShow (L"ID_DFR0", GoyaDbgIdDfr0, 0, FALSE, NULL);
  DbgShow (L"DIDR", GoyaDbgDidr, 0, FALSE, NULL);
  DbgShow (L"DRAR", GoyaDbgDrar, 0, FALSE, NULL);
  DbgShow (L"DSAR", GoyaDbgDsar, 0, FALSE, NULL);
  Print (L"\n ");
  DbgShow (L"OSLSR", GoyaDbgOslsr, 0, FALSE, &Oslsr);
  if (Oslsr & 2) {
    DbgShow (L"OSLAR", GoyaDbgOslarW, 0, TRUE, NULL);
    DbgShow (L"OSLSR", GoyaDbgOslsr, 0, FALSE, &Oslsr);
  }
  DbgShow (L"PRSR", GoyaDbgPrsr, 0, FALSE, &Prsr);
  DbgShow (L"PRSR", GoyaDbgPrsr, 0, FALSE, &V);
  Print (L" (PU=%u SPD=%u)\n ", V & 1, (V >> 1) & 1);
  DbgShow (L"DSCRint", GoyaDbgDscrInt, 0, FALSE, NULL);
  DbgShow (L"DSCRext", GoyaDbgDscrExt, 0, FALSE, &Dscr);
  if (mDbgFault == 0) {
    DbgShow (L"DSCRext", GoyaDbgDscrExtW, Dscr | 0x1000, TRUE, NULL);
  }
  Print (L"\n ");
  DbgShow (L"VCR", GoyaDbgVcrW, 0, TRUE, NULL);
  DbgShow (L"BCR0", GoyaDbgBcr0, 0, FALSE, NULL);
  DbgShow (L"BCR0", GoyaDbgBcr0W, 0, TRUE, NULL);
  DbgShow (L"BVR0", GoyaDbgBvr0W, 0, TRUE, NULL);
  DbgShow (L"WCR0", GoyaDbgWcr0W, 0, TRUE, NULL);
  DbgShow (L"WFAR", GoyaDbgWfar, 0, FALSE, NULL);
  Print (L"\n");

Done:
  if (Undef) {
    Cpu->RegisterInterruptHandler (Cpu, EXCEPT_ARM_UNDEFINED_INSTRUCTION, NULL);
  }

  if (Abort) {
    Cpu->RegisterInterruptHandler (Cpu, EXCEPT_ARM_DATA_ABORT, NULL);
  }
}

STATIC
VOID
ShowSystem (
  VOID
  )
{
  EFI_MEMORY_DESCRIPTOR  *Map = NULL;
  UINTN                  MapSize = 0;
  UINTN                  MapKey;
  UINTN                  DescSize;
  UINT32                 DescVer;
  UINT64                 Conventional = 0;
  UINT64                 Total = 0;
  UINT8                  *P;
  UINT32                 Midr;
  UINT32                 Typer;

  Print (L"Firmware %s, UEFI %u.%u, rev 0x%x\n", gST->FirmwareVendor,
         gST->Hdr.Revision >> 16, gST->Hdr.Revision & 0xFFFF, gST->FirmwareRevision);

  Midr = ArmReadMidr ();
  Print (L"CPU   MIDR %08x (%s r%up%u)  MPIDR %08x\n", Midr,
         ((Midr >> 4) & 0xFFF) == 0xC09 ? L"Cortex-A9" : L"unknown",
         (Midr >> 20) & 0xF, Midr & 0xF, ArmReadMpidr ());

  ShowTimerIrq ();
  ShowCpuDebug ();

  Typer = MmioRead32 (PXA_GIC_D + GICD_TYPER);
  Print (L"GIC   %u IRQs, %u CPU(s), GICD_CTLR %x GICC_CTLR %x, IIDR %08x / %08x\n",
         ((Typer & 0x1F) + 1) * 32, ((Typer >> 5) & 7) + 1,
         MmioRead32 (PXA_GIC_D + GICD_CTLR), MmioRead32 (PXA_GIC_C + GICC_CTLR),
         MmioRead32 (PXA_GIC_D + GICD_IIDR), MmioRead32 (PXA_GIC_C + GICC_IIDR));
  Print (L"ICU   IRQ mask %x, FIQ mask %x (3 = masked, GIC is used)\n",
         MmioRead32 (PXA988_ICU_BASE + 0x114), MmioRead32 (PXA988_ICU_BASE + 0x144));

  gBS->GetMemoryMap (&MapSize, Map, &MapKey, &DescSize, &DescVer);
  MapSize += 4 * DescSize;
  Map = AllocatePool (MapSize);
  if ((Map != NULL) && !EFI_ERROR (gBS->GetMemoryMap (&MapSize, Map, &MapKey, &DescSize, &DescVer))) {
    for (P = (UINT8 *)Map; P < (UINT8 *)Map + MapSize; P += DescSize) {
      EFI_MEMORY_DESCRIPTOR  *D = (EFI_MEMORY_DESCRIPTOR *)P;
      Total += D->NumberOfPages;
      if (D->Type == EfiConventionalMemory) {
        Conventional += D->NumberOfPages;
      }
    }

    Print (L"RAM   %lu MB in the UEFI map, %lu MB free\n", (Total * 4096) >> 20, (Conventional * 4096) >> 20);
  }

  FreePool (Map);
  Print (L"Cores (Cpu1Status):\n");
  ShowVariable (L"Cpu1Status", L"no Cpu1Status (driver disabled?)");
}

STATIC
VOID
ShowPmic (
  VOID
  )
{
  UINT8  Id;
  UINT8  Sts;
  UINT8  Ldo;

  if (EFI_ERROR (Pm822Read (PM822_I2C_ADDR, 0x00, &Id))) {
    Print (L"PMIC  88PM822 not answering on TWSI2 0x%02x\n", PM822_I2C_ADDR);
    return;
  }

  Pm822Read (PM822_I2C_ADDR, PM800_STATUS_1, &Sts);
  Pm822Read (PM822_POWER_I2C_ADDR, PM800_LDO_ENA1_1, &Ldo);
  Print (L"PMIC  88PM822 chip id 0x%02x, STATUS_1 0x%02x (onkey %s), LDO_ENA1_1 0x%02x\n", Id, Sts,
         (Sts & PM800_ONKEY_STS) ? L"down" : L"up", Ldo);
  Print (L"      microSD supplies: LDO4 %s, LDO5 %s\n", (Ldo & PM800_LDO4_EN) ? L"on" : L"off", (Ldo & PM800_LDO5_EN) ? L"on" : L"off");
}

STATIC
VOID
ShowDisplay (
  VOID
  )
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;

  if (EFI_ERROR (gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop))) {
    Print (L"GOP   none\n");
    return;
  }

  Print (L"GOP   %ux%u, %u px/line, FB 0x%lx (%lu KB), %u mode(s)\n",
         Gop->Mode->Info->HorizontalResolution, Gop->Mode->Info->VerticalResolution,
         Gop->Mode->Info->PixelsPerScanLine, (UINT64)Gop->Mode->FrameBufferBase,
         (UINT64)Gop->Mode->FrameBufferSize >> 10, Gop->Mode->MaxMode);
}

STATIC
VOID
ShowOneSd (
  IN CONST CHAR16  *Name,
  IN UINTN         Base,
  IN UINTN         ApmuClk,
  IN UINT8         CdGpio      // 0xFF = none
  )
{
  //
  // Each controller needs its own reset releases and function clock; the AXI
  // clock enable only exists in the SDH0 register and is shared by all.
  //
  UINT32  Need = SDH_CLK_RST_N | SDH_CLK_AXI_RST_N | SDH_CLK_FCLK_EN;
  UINT32  C0;
  UINT32  C1;
  UINT32  Ps;
  UINT8   Hc1;
  UINT16  Clk;
  UINT32  Div;
  UINT32  Khz;

  if (((MmioRead32 (ApmuClk) & Need) != Need) || ((MmioRead32 (APMU_SDH0) & SDH_CLK_ACLK_EN) == 0)) {
    Print (L"%s clock off (APMU %08x / SDH0 %08x), not touched\n", Name, MmioRead32 (ApmuClk), MmioRead32 (APMU_SDH0));
    return;
  }

  C0  = MmioRead32 (Base + SDHCI_CAPS0);
  C1  = MmioRead32 (Base + SDHCI_CAPS1);
  Ps  = MmioRead32 (Base + SDHCI_PRESENT_STATE);
  Hc1 = MmioRead8 (Base + SDHCI_HOST_CTRL1);
  Clk = MmioRead16 (Base + SDHCI_CLOCK_CTRL);
  Div = ((Clk >> 8) & 0xFF) | (((Clk >> 6) & 3) << 8);
  Khz = PXA988_SDH_BASE_CLOCK / 1000;
  Khz = Div == 0 ? Khz : Khz / (2 * Div);

  Print (L"%s caps %08x/%08x (HS %s, 1.8V %s, 8-bit %s), state %08x: card %s\n", Name, C0, C1,
         (C0 & BIT21) ? L"yes" : L"no", (C0 & BIT26) ? L"yes" : L"no", (C0 & BIT18) ? L"yes" : L"no", Ps,
         (Ps & BIT16) ? L"present" : L"absent");
  if (Clk & BIT2) {
    Print (L"      bus now: %u kHz, %u-bit, HS %s, clock running", Khz,
           (Hc1 & BIT5) ? 8 : ((Hc1 & BIT1) ? 4 : 1), (Hc1 & BIT2) ? L"on" : L"off");
  } else {
    Print (L"      bus now: idle (SD clock stopped), %u-bit, HS %s", (Hc1 & BIT5) ? 8 : ((Hc1 & BIT1) ? 4 : 1),
           (Hc1 & BIT2) ? L"on" : L"off");
  }
  if (CdGpio != 0xFF) {
    Print (L", CD pin GPIO%u = %u", CdGpio, (MmioRead32 (GPIO_BANK (CdGpio) + GPIO_GPLR) >> (CdGpio % 32)) & 1);
  }

  Print (L"\n");
}

STATIC
VOID
ShowStorage (
  VOID
  )
{
  EFI_HANDLE             *Handles;
  UINTN                  Count;
  UINTN                  Index;
  EFI_BLOCK_IO_PROTOCOL  *BlockIo;
  UINTN                  Shown = 0;

  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &Count, &Handles))) {
    Print (L"Disks none\n");
    return;
  }

  for (Index = 0; Index < Count; Index++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo)) ||
        BlockIo->Media->LogicalPartition) {
      continue;
    }

    Print (L"Disk  %s: %lu MB, %u-byte blocks%s%s\n",
           BlockIo->Media->RemovableMedia ? L"removable" : L"fixed    ",
           MultU64x32 (BlockIo->Media->LastBlock + 1, BlockIo->Media->BlockSize) >> 20,
           BlockIo->Media->BlockSize,
           BlockIo->Media->ReadOnly ? L", read-only" : L"",
           BlockIo->Media->MediaPresent ? L"" : L", no media");
    Shown++;
  }

  if (Shown == 0) {
    Print (L"Disks none\n");
  }

  FreePool (Handles);
}

STATIC
VOID
ShowUsbRegs (
  VOID
  )
{
  UINTN   Op;
  UINT32  Portsc;

  if ((MmioRead32 (APMU_USB) & APMU_USB_CLK_EN) != APMU_USB_CLK_EN) {
    Print (L"  USB clock off (APMU %08x), registers not touched\n", MmioRead32 (APMU_USB));
    return;
  }

  Op     = PXA988_USB_EHCI_BASE + MmioRead8 (PXA988_USB_EHCI_BASE);
  Portsc = MmioRead32 (Op + 0x44);
  Print (L"  USBCMD %08x USBSTS %08x USBMODE %08x (3 = host)\n", MmioRead32 (Op), MmioRead32 (Op + 4),
         MmioRead32 (PXA988_USB_EHCI_BASE + USB_EHCI_USBMODE));
  Print (L"  PORTSC %08x: device %s, enabled %s, power %s, OTGSC %08x\n", Portsc,
         (Portsc & BIT0) ? L"connected" : L"none", (Portsc & BIT2) ? L"yes" : L"no",
         (Portsc & BIT12) ? L"on" : L"off", MmioRead32 (PXA988_USB_BASE + 0x1A4));
}

STATIC
BOOLEAN
UsbWait (
  IN UINTN   Address,
  IN UINT32  Mask,
  IN UINT32  Value
  )
{
  UINTN  Ms;

  for (Ms = 0; Ms < 100; Ms++) {
    if ((MmioRead32 (Address) & Mask) == Value) {
      return TRUE;
    }

    gBS->Stall (1000);
  }

  return FALSE;
}

/**
  Controller reset, then report what the mode register does. Resets the USB
  controller underneath the USB drivers, so the page tells the user to reboot.
**/
STATIC
BOOLEAN
UsbResetController (
  IN UINTN  Op
  )
{
  MmioAnd32 (Op, ~(UINT32)BIT0);                    // RS = 0
  UsbWait (Op + 4, BIT12, BIT12);                   // halted
  MmioOr32 (Op, BIT1);                              // RST
  return UsbWait (Op, BIT1, 0);
}

STATIC
VOID
UsbProbe (
  VOID
  )
{
  UINTN   Op;
  UINT32  Mode;

  gST->ConOut->ClearScreen (gST->ConOut);
  Print (L"GoyaWifiDiag  USB host probe\n\n");
  if ((MmioRead32 (APMU_USB) & APMU_USB_CLK_EN) != APMU_USB_CLK_EN) {
    Print (L"USB clock off, nothing to probe\n");
    return;
  }

  Print (L"This resets the USB controller under the USB drivers: reboot afterwards.\n");
  Print (L"Unplug the PC cable first if you want a host (OTG) result.\n\n");

  Op = PXA988_USB_EHCI_BASE + MmioRead8 (PXA988_USB_EHCI_BASE);
  Print (L"DCCPARAMS %08x (bit7 host capable: %s, bit8 device capable: %s, endpoints %u)\n",
         MmioRead32 (PXA988_USB_BASE + 0x124),
         (MmioRead32 (PXA988_USB_BASE + 0x124) & BIT7) ? L"yes" : L"no",
         (MmioRead32 (PXA988_USB_BASE + 0x124) & BIT8) ? L"yes" : L"no",
         MmioRead32 (PXA988_USB_BASE + 0x124) & 0x1F);
  Print (L"before : USBMODE %08x  OTGSC %08x  PORTSC %08x  USBSTS %08x\n", MmioRead32 (PXA988_USB_EHCI_BASE + USB_EHCI_USBMODE),
         MmioRead32 (PXA988_USB_BASE + 0x1A4), MmioRead32 (Op + 0x44), MmioRead32 (Op + 4));

  Print (L"reset  : %s, USBMODE %08x\n", UsbResetController (Op) ? L"done" : L"TIMEOUT",
         MmioRead32 (PXA988_USB_EHCI_BASE + USB_EHCI_USBMODE));

  MmioWrite32 (PXA988_USB_EHCI_BASE + USB_EHCI_USBMODE, 0x3);
  Mode = MmioRead32 (PXA988_USB_EHCI_BASE + USB_EHCI_USBMODE);
  Print (L"try 1  : write 3 (host)          -> USBMODE %08x %s\n", Mode, (Mode & 3) == 3 ? L"HOST" : L"no");

  UsbResetController (Op);
  MmioWrite32 (PXA988_USB_EHCI_BASE + USB_EHCI_USBMODE, 0xB);
  Mode = MmioRead32 (PXA988_USB_EHCI_BASE + USB_EHCI_USBMODE);
  Print (L"try 2  : write 0xB (host, SLOM)  -> USBMODE %08x %s\n", Mode, (Mode & 3) == 3 ? L"HOST" : L"no");

  UsbResetController (Op);
  MmioOr32 (PXA988_USB_BASE + 0x1A4, BIT5);         // OTGSC.IDPU: ID pull-up on
  gBS->Stall (10000);
  Print (L"ID pull : OTGSC %08x (bit8 = ID pin, 1 = no OTG cable)\n", MmioRead32 (PXA988_USB_BASE + 0x1A4));
  MmioWrite32 (PXA988_USB_EHCI_BASE + USB_EHCI_USBMODE, 0x3);
  Mode = MmioRead32 (PXA988_USB_EHCI_BASE + USB_EHCI_USBMODE);
  Print (L"try 3  : ID pull-up, write 3     -> USBMODE %08x %s\n", Mode, (Mode & 3) == 3 ? L"HOST" : L"no");

  if ((Mode & 3) == 3) {
    MmioOr32 (Op + 0x44, BIT12);                    // PORTSC.PP
    gBS->Stall (50000);
    Print (L"port   : PP set, PORTSC %08x, OTGSC %08x (AVV bit9, ASV bit10)\n", MmioRead32 (Op + 0x44),
           MmioRead32 (PXA988_USB_BASE + 0x1A4));
  }

  Print (L"\nHost mode %s.\n", (Mode & 3) == 3 ? L"is accepted by the controller" : L"was refused in every attempt");
}

STATIC
VOID
ColorTest (
  VOID
  )
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL   *Gop;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Color[] = {
    { 0, 0, 255, 0 }, { 0, 255, 0, 0 }, { 255, 0, 0, 0 }, { 255, 255, 255, 0 }, { 0, 0, 0, 0 }, { 128, 128, 128, 0 }
  };
  EFI_INPUT_KEY                  Key;
  UINTN                          Index;
  UINTN                          I;

  if (EFI_ERROR (gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop))) {
    return;
  }

  for (I = 0; I < ARRAY_SIZE (Color); I++) {
    Gop->Blt (Gop, &Color[I], EfiBltVideoFill, 0, 0, 0, 0, Gop->Mode->Info->HorizontalResolution, Gop->Mode->Info->VerticalResolution, 0);
    gST->ConIn->Reset (gST->ConIn, FALSE);
    for ( ; ; ) {
      gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &Index);
      if (!EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key))) {
        break;
      }
    }

    if (Key.ScanCode == SCAN_ESC) {
      break;
    }

  }

  gST->ConOut->ClearScreen (gST->ConOut);
}

STATIC
VOID
KeyTest (
  VOID
  )
{
  EFI_INPUT_KEY  Key;
  BOOLEAN        PowerOnce = FALSE;
  BOOLEAN        Prev[KEY_COUNT] = { FALSE };
  UINTN          I;
  UINTN          Idle = 0;

  Print (L"Key test: raw levels on the left, UEFI key events below. Power twice = exit.\n\n");
  gST->ConIn->Reset (gST->ConIn, FALSE);

  for ( ; ; ) {
    BOOLEAN  Changed = FALSE;

    for (I = 0; I < KEY_COUNT; I++) {
      BOOLEAN  Down = RawKeyDown (&mRawKeys[I]);
      if (Down != Prev[I]) {
        Prev[I] = Down;
        Changed = TRUE;
      }
    }

    if (Changed) {
      Print (L"  raw:");
      for (I = 0; I < KEY_COUNT; I++) {
        Print (L" %s=%s", mRawKeys[I].Name, Prev[I] ? L"DOWN" : L"up  ");
      }

      Print (L"\n");
    }

    if (!EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key))) {
      CONST CHAR16  *Name = L"other";
      if (Key.ScanCode == SCAN_UP) {
        Name = L"Vol+ (Up)";
      } else if (Key.ScanCode == SCAN_DOWN) {
        Name = L"Vol- (Down)";
      } else if (Key.ScanCode == SCAN_ESC) {
        Name = L"Power (Esc)";
      } else if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
        Name = L"Home (Enter)";
      }

      Print (L"  key event: %s\n", Name);
      if (Key.ScanCode == SCAN_ESC) {
        if (PowerOnce) {
          return;
        }

        PowerOnce = TRUE;
      } else {
        PowerOnce = FALSE;
      }
    }

    gBS->Stall (20000);
    if (++Idle > 3000) {          // 60 s without leaving: give the menu back
      return;
    }
  }
}

EFI_STATUS
EFIAPI
GoyaWifiDiagMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  gST->ConOut->ClearScreen (gST->ConOut);
  Print (L"GoyaWifiDiag - Galaxy Tab 3 Lite 7.0 Wi-Fi (SM-T110, PXA986)  page 1/5: system\n\n");
  ShowSystem ();
  if (!NextPage (L"next page")) {
    return EFI_SUCCESS;
  }

  gST->ConOut->ClearScreen (gST->ConOut);
  Print (L"GoyaWifiDiag  page 2/5: board\n\n");
  ShowPmic ();
  ShowDisplay ();
  ShowOneSd (L"SDH1 microSD", PXA988_SDH1_BASE, APMU_SDH0, 11);
  ShowOneSd (L"SDH3 eMMC   ", PXA988_SDH3_BASE, APMU_SDH2, 0xFF);
  ShowStorage ();
  if (!NextPage (L"next page")) {
    return EFI_SUCCESS;
  }

  gST->ConOut->ClearScreen (gST->ConOut);
  Print (L"GoyaWifiDiag  page 3/5: bring-up drivers\n\n");
  Print (L"CPU1 (Pxa988SmpDxe):\n");
  ShowVariable (L"Cpu1Status", L"no Cpu1Status variable (driver disabled or it did not run)");
  Print (L"\nUSB host (Pxa988UsbDxe):\n");
  ShowVariable (L"UsbStatus", L"no UsbStatus variable (driver disabled or it did not run)");
  ShowUsbRegs ();
  Print (L"\n[Vol+] USB host probe (resets the USB controller)\n");
  for ( ; ; ) {
    EFI_INPUT_KEY  Key;
    UINTN          Index;

    Print (L"[Home] colour test (any key = next colour)    [Power] quit\n");
    gST->ConIn->Reset (gST->ConIn, FALSE);
    do {
      gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &Index);
    } while (EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key)) ||
             ((Key.ScanCode != SCAN_UP) && (Key.ScanCode != SCAN_ESC) && (Key.UnicodeChar != CHAR_CARRIAGE_RETURN)));
    if (Key.ScanCode == SCAN_ESC) {
      return EFI_SUCCESS;
    }

    if (Key.ScanCode != SCAN_UP) {
      break;
    }

    UsbProbe ();
  }

  ColorTest ();
  Print (L"GoyaWifiDiag  page 5/5: keys\n\n");
  KeyTest ();
  gST->ConOut->ClearScreen (gST->ConOut);
  return EFI_SUCCESS;
}
