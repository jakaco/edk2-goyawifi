/** @file
  PXA988 / PXA986 USB 2.0 host bring-up.

  The controller is a ChipIdea-style OTG core (0xD4208000) behind a Marvell
  UTMI PHY (0xD4207000). The vendor kernels only ever run it as a device; this
  driver sets it up for host mode as an experiment:

    - APMU USB clock on (also releases the AXI reset)
    - PHY power and calibration, values from the vendor kernel
      (board-goya.c usb_phy_init_internal(), same table U-Boot uses)
    - controller reset, then USBMODE = host
    - registered as a non-discoverable EHCI device (EHCI registers start at
      0xD4208100); ChipIdeaEhciDxe re-selects host mode after its own reset

  Not handled here: VBUS. The board has no 5 V source this driver knows how to
  switch (the SMB328 charger has an OTG bit, not wired up yet), so devices must
  be self-powered or sit behind a powered hub / OTG Y cable.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>
#include <Library/PcdLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Pxa988.h>

#define PHY_PLL_CTRL1     0x08
#define PHY_PLL_READY     BIT15
#define PHY_POWER_CTRL    0x104
#define PHY_POWER_ON      0x10901003

#define USBCMD_RS         BIT0
#define USBCMD_RST        BIT1
#define USBSTS_HCH        BIT12

STATIC CONST struct {
  UINT16    Offset;
  UINT16    Value;
} mPhyRegs[] = {
  { 0x04, 0x5af0 },       // PLLVDD18=1, REFDIV=13, FBDIV=240
  { 0x08, 0x3133 },       // PU_PLL, LOCK_BYPASS, ICP=1, KVCO=3, CAL12=3
  { 0x10, 0x0188 },       // IMPCAL_VTH=1, HS/FS_RCAL=8
  { 0x14, 0x07d4 },       // TXVDD15=1, TXVDD12=3, LOWVDD, AMP=5, PHSEL=4
  { 0x18, 0x0aff },       // SLEWRATE=2, CAL_DLY=2, FS/HS_DRV=15
  { 0x20, 0xaac1 },       // SQ_THRESH=12 (goya)
  { 0x24, 0x3892 },
  { 0x28, 0x0105 },
  { 0x30, 0x0110 },
  { 0x34, 0x5680 },
  { 0x3c, 0x2586 },
  { 0x40, 0xe400 },
  { 0x58, 0x0000 },
};

STATIC
BOOLEAN
WaitBits (
  IN UINTN   Address,
  IN UINT32  Mask,
  IN UINT32  Value,
  IN UINTN   TimeoutMs
  )
{
  while (TimeoutMs-- > 0) {
    if ((MmioRead32 (Address) & Mask) == Value) {
      return TRUE;
    }

    MicroSecondDelay (1000);
  }

  return (MmioRead32 (Address) & Mask) == Value;
}

EFI_STATUS
EFIAPI
Pxa988UsbInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle;
  UINTN       Index;
  UINTN       OpBase;

  if (!FixedPcdGetBool (PcdUsbHostEnable)) {
    return EFI_SUCCESS;
  }

  MmioWrite32 (APMU_USB, APMU_USB_CLK_EN);
  MmioRead32 (APMU_USB);
  MicroSecondDelay (20);

  MmioWrite32 (PXA988_USB_PHY_BASE + PHY_POWER_CTRL, PHY_POWER_ON);
  for (Index = 0; Index < ARRAY_SIZE (mPhyRegs); Index++) {
    MmioWrite16 (PXA988_USB_PHY_BASE + mPhyRegs[Index].Offset, mPhyRegs[Index].Value);
  }

  if (!WaitBits (PXA988_USB_PHY_BASE + PHY_PLL_CTRL1, PHY_PLL_READY, PHY_PLL_READY, 200)) {
    DEBUG ((DEBUG_WARN, "Pxa988Usb: PHY PLL not ready (0x%04x), continuing\n", MmioRead16 (PXA988_USB_PHY_BASE + PHY_PLL_CTRL1)));
  }

  //
  // Reset the controller into a known state, then pick host mode
  //
  OpBase = PXA988_USB_EHCI_BASE + MmioRead8 (PXA988_USB_EHCI_BASE);        // + CAPLENGTH
  DEBUG ((
    DEBUG_INFO,
    "Pxa988Usb: HCIVERSION=0x%04x HCSPARAMS=0x%08x CAPLENGTH=0x%02x\n",
    MmioRead16 (PXA988_USB_EHCI_BASE + 2),
    MmioRead32 (PXA988_USB_EHCI_BASE + 4),
    MmioRead8 (PXA988_USB_EHCI_BASE)
    ));

  MmioAnd32 (OpBase, ~(UINT32)USBCMD_RS);
  WaitBits (OpBase + 4, USBSTS_HCH, USBSTS_HCH, 20);
  MmioOr32 (OpBase, USBCMD_RST);
  if (!WaitBits (OpBase, USBCMD_RST, 0, 100)) {
    DEBUG ((DEBUG_ERROR, "Pxa988Usb: controller reset timed out\n"));
    return EFI_SUCCESS;
  }

  MmioWrite32 (PXA988_USB_EHCI_BASE + USB_EHCI_USBMODE, USBMODE_CM_HOST);
  DEBUG ((DEBUG_INFO, "Pxa988Usb: USBMODE=0x%08x\n", MmioRead32 (PXA988_USB_EHCI_BASE + USB_EHCI_USBMODE)));

  Handle = NULL;
  Status = RegisterNonDiscoverableMmioDevice (
             NonDiscoverableDeviceTypeEhci,
             NonDiscoverableDeviceDmaTypeNonCoherent,
             NULL,
             &Handle,
             1,
             PXA988_USB_EHCI_BASE,
             PXA988_USB_EHCI_SIZE
             );
  DEBUG ((DEBUG_INFO, "Pxa988Usb: EHCI registered: %r\n", Status));

  return EFI_SUCCESS;
}
