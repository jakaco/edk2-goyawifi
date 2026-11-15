/** @file
  PXA988 SDHCI glue for SdMmcPciHcDxe.

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
#include <Protocol/SdMmcOverride.h>
#include <Pxa988.h>

//
// Copy of the private SD_MMC_HC_SLOT_CAP layout from SdMmcPciHcDxe
//
typedef struct {
  UINT32    TimeoutFreq   : 6;
  UINT32    Reserved      : 1;
  UINT32    TimeoutUnit   : 1;
  UINT32    BaseClkFreq   : 8;
  UINT32    MaxBlkLen     : 2;
  UINT32    BusWidth8     : 1;
  UINT32    Adma2         : 1;
  UINT32    Reserved2     : 1;
  UINT32    HighSpeed     : 1;
  UINT32    Sdma          : 1;
  UINT32    SuspRes       : 1;
  UINT32    Voltage33     : 1;
  UINT32    Voltage30     : 1;
  UINT32    Voltage18     : 1;
  UINT32    SysBus64V4    : 1;
  UINT32    SysBus64V3    : 1;
  UINT32    AsyncInt      : 1;
  UINT32    SlotType      : 2;
  UINT32    Sdr50         : 1;
  UINT32    Sdr104        : 1;
  UINT32    Ddr50         : 1;
  UINT32    UhsII         : 1;
  UINT32    DriverTypeA   : 1;
  UINT32    DriverTypeC   : 1;
  UINT32    DriverTypeD   : 1;
  UINT32    DriverType4   : 1;
  UINT32    TimerCount    : 4;
  UINT32    Reserved4     : 1;
  UINT32    TuningSDR50   : 1;
  UINT32    RetuningMod   : 2;
  UINT32    ClkMultiplier : 8;
  UINT32    Reserved5     : 3;
  UINT32    Adma3         : 1;
  UINT32    Vdd2Voltage18 : 1;
  UINT32    Reserved6     : 2;
  UINT32    Hs400         : 1;
} PXA_SD_MMC_HC_SLOT_CAP;

//
// PXAv3 vendor register: sampling clock delay (Linux "mrvl,clk-delay-cycles")
//
#define SD_CLOCK_BURST_SIZE_SETUP  0x10A
#define   SDCLK_SEL                BIT8
#define   SDCLK_DELAY_SHIFT        9
#define   SDCLK_DELAY_MASK         0x1F

#define SD_HOST_CTRL1              0x28
#define   HOST_CTRL1_CD_TEST_LVL   BIT6
#define   HOST_CTRL1_CD_SIG_SEL    BIT7

typedef struct {
  UINTN         Base;
  UINT32        MaskBit;
  BOOLEAN       IsEmmc;
  UINT32        MaxClockMhz;
  UINT32        ClkDelayCycles;
  EFI_HANDLE    Handle;
} PXA_SDHCI;

STATIC PXA_SDHCI  mSdhci[] = {
  // eMMC: 8-bit, no 1.8 V, 26 MHz max, clk-delay 15 (from the goyawifi DT)
  { PXA988_SDH3_BASE, BIT2, TRUE,  26, 15, NULL },
  // microSD: 4-bit, no 1.8 V (no UHS), 50 MHz max
  { PXA988_SDH1_BASE, BIT0, FALSE, 50, 0,  NULL },
};

STATIC
PXA_SDHCI *
FindSdhci (
  IN EFI_HANDLE  Handle
  )
{
  UINTN  Index;

  for (Index = 0; Index < ARRAY_SIZE (mSdhci); Index++) {
    if ((mSdhci[Index].Handle != NULL) && (mSdhci[Index].Handle == Handle)) {
      return &mSdhci[Index];
    }
  }

  return NULL;
}

/**
  The microSD card-detect line is a GPIO (11, active low) on goyawifi, not
  the controller CD pin, so feed its state to the controller through the
  "card detect test level" override in Host Control 1.
**/
STATIC
VOID
PxaSdForceCardDetect (
  IN PXA_SDHCI  *Host
  )
{
  UINT8    Ctrl;
  BOOLEAN  Present;

  Present = (MmioRead32 (GPIO_BANK (GOYAWIFI_SD_CD_GPIO) + GPIO_GPLR) &
             (1U << (GOYAWIFI_SD_CD_GPIO % 32))) == 0;

  Ctrl  = MmioRead8 (Host->Base + SD_HOST_CTRL1);
  Ctrl |= HOST_CTRL1_CD_SIG_SEL;
  if (Present) {
    Ctrl |= HOST_CTRL1_CD_TEST_LVL;
  } else {
    Ctrl &= ~HOST_CTRL1_CD_TEST_LVL;
  }

  MmioWrite8 (Host->Base + SD_HOST_CTRL1, Ctrl);
}

STATIC
EFI_STATUS
EFIAPI
PxaSdMmcCapability (
  IN      EFI_HANDLE  ControllerHandle,
  IN      UINT8       Slot,
  IN OUT  VOID        *SdMmcHcSlotCapability,
  IN OUT  UINT32      *BaseClkFreq
  )
{
  PXA_SD_MMC_HC_SLOT_CAP  *Cap;
  PXA_SDHCI               *Host;

  Host = FindSdhci (ControllerHandle);
  if ((Host == NULL) || (Slot != 0)) {
    return EFI_NOT_FOUND;
  }

  Cap = SdMmcHcSlotCapability;

  // Base clock does not come from the capability register on PXAv3
  *BaseClkFreq     = PXA988_SDH_BASE_CLOCK / 1000000;
  Cap->BaseClkFreq = PXA988_SDH_BASE_CLOCK / 1000000;

  // No 1.8 V signalling on this board (no-1-8-v), so no UHS / HS200 / HS400
  Cap->Voltage18     = 0;
  Cap->Vdd2Voltage18 = 0;
  Cap->Sdr50         = 0;
  Cap->Sdr104        = 0;
  Cap->Ddr50         = 0;
  Cap->Hs400         = 0;
  Cap->SysBus64V3    = 0;
  Cap->SysBus64V4    = 0;
  Cap->Voltage33     = 1;
  Cap->HighSpeed     = 1;
  Cap->BusWidth8     = Host->IsEmmc ? 1 : 0;
  Cap->SlotType      = Host->IsEmmc ? 1 : 0;   // embedded : removable

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
PxaSdMmcNotifyPhase (
  IN      EFI_HANDLE               ControllerHandle,
  IN      UINT8                    Slot,
  IN      EDKII_SD_MMC_PHASE_TYPE  PhaseType,
  IN OUT  VOID                     *PhaseData
  )
{
  PXA_SDHCI                          *Host;
  EDKII_SD_MMC_OPERATING_PARAMETERS  *Params;
  UINT16                             Val;

  Host = FindSdhci (ControllerHandle);
  if ((Host == NULL) || (Slot != 0)) {
    return EFI_NOT_FOUND;
  }

  switch (PhaseType) {
    case EdkiiSdMmcInitHostPre:
      if (!Host->IsEmmc) {
        PxaSdForceCardDetect (Host);
      }

      break;

    case EdkiiSdMmcResetPost:
    case EdkiiSdMmcInitHostPost:
      if (Host->ClkDelayCycles != 0) {
        Val  = MmioRead16 (Host->Base + SD_CLOCK_BURST_SIZE_SETUP);
        Val &= ~(UINT16)(SDCLK_DELAY_MASK << SDCLK_DELAY_SHIFT);
        Val |= (UINT16)((Host->ClkDelayCycles & SDCLK_DELAY_MASK) << SDCLK_DELAY_SHIFT);
        Val |= SDCLK_SEL;
        MmioWrite16 (Host->Base + SD_CLOCK_BURST_SIZE_SETUP, Val);
      }

      break;

    case EdkiiSdMmcGetOperatingParam:
      Params            = PhaseData;
      Params->ClockFreq = Host->MaxClockMhz;
      break;

    default:
      break;
  }

  return EFI_SUCCESS;
}

STATIC EDKII_SD_MMC_OVERRIDE  mSdMmcOverride = {
  EDKII_SD_MMC_OVERRIDE_PROTOCOL_VERSION,
  PxaSdMmcCapability,
  PxaSdMmcNotifyPhase
};

STATIC
VOID
Pxa988SdhClockInit (
  VOID
  )
{
  CONST UINT32  Val = SDH_CLK_RST_N | SDH_CLK_AXI_RST_N | SDH_CLK_FCLK_EN |
                      SDH_CLK_DIV (7);

  //
  // pll1_416 / (7 + 1) = 52 MHz on every controller, matching the override
  // above. The shared SDH AXI clock enable lives in the SDH0 register.
  //
  MmioWrite32 (APMU_SDH0, Val | SDH_CLK_ACLK_EN | SDH_CLK_FC);
  MmioWrite32 (APMU_SDH1, Val | SDH_CLK_FC);
  MmioWrite32 (APMU_SDH2, Val | SDH_CLK_FC);
  MicroSecondDelay (10);
}

STATIC
VOID
Pxa988SdPinmux (
  VOID
  )
{
  STATIC CONST UINT32  SdPads[] = {
    MFPR_MMC1_DAT0, MFPR_MMC1_DAT1, MFPR_MMC1_DAT2, MFPR_MMC1_DAT3,
    MFPR_MMC1_CMD,  MFPR_MMC1_CLK,  MFPR_GPIO11
  };
  UINTN                Index;

  // All AF0: MMC1 function on the MMC1 pads, GPIO function on GPIO11
  for (Index = 0; Index < ARRAY_SIZE (SdPads); Index++) {
    MmioAnd32 (PXA988_MFPR_BASE + SdPads[Index], ~(UINT32)MFPR_AF_MASK);
  }

  // GPIO11 as input
  MmioWrite32 (
    GPIO_BANK (GOYAWIFI_SD_CD_GPIO) + GPIO_GCDR,
    1U << (GOYAWIFI_SD_CD_GPIO % 32)
    );
}

EFI_STATUS
EFIAPI
Pxa988SdhciInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle;
  UINTN       Index;

  Pxa988SdhClockInit ();
  Pxa988SdPinmux ();

  for (Index = 0; Index < ARRAY_SIZE (mSdhci); Index++) {
    if ((FixedPcdGet32 (PcdSdhciMask) & mSdhci[Index].MaskBit) == 0) {
      continue;
    }

    Status = RegisterNonDiscoverableMmioDevice (
               NonDiscoverableDeviceTypeSdhci,
               NonDiscoverableDeviceDmaTypeNonCoherent,
               NULL,
               &mSdhci[Index].Handle,
               1,
               mSdhci[Index].Base,
               PXA988_SDH_SIZE
               );
    DEBUG ((DEBUG_INFO, "Pxa988Sdhci: 0x%x: %r\n", mSdhci[Index].Base, Status));
  }

  Handle = NULL;
  return gBS->InstallMultipleProtocolInterfaces (
                &Handle,
                &gEdkiiSdMmcOverrideProtocolGuid,
                &mSdMmcOverride,
                NULL
                );
}
