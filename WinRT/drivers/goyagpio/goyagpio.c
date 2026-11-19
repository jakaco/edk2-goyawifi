/* Marvell PXA986/PXA988 GPIO controller - GpioClx client driver (Windows 8.0 RT / Windows 10).

   Ported from drivers/gpio/gpio-pxa.c of the Samsung SM-T110 (goyawifi) 4.19 kernel, using
   the structure of Microsoft's imx-iotcore imxgpio GpioClx driver (MIT).

   Controller:  0xD4019000, 4 banks x 32 pins = 128 GPIOs, one muxed interrupt (SPI 49 = GIC 81).
   Per-bank registers (bank offset = ((n / 3) << 8) + ((n % 3) << 2)):
       GPLR 0x00  GPDR 0x0C  GPSR 0x18  GPCR 0x24  GRER 0x30  GFER 0x3C  GEDR 0x48  ED_MASK 0x9C
   Only edge interrupts exist in the hardware (no level mode).

   Not done in this version: pinmux (MFPR at 0xD401E000) and pull configuration. Pins are used
   as the bootloader/firmware left them; the firmware already sets the pins Windows needs
   (keys, touch IRQ) as GPIO inputs.
*/
#include <ntddk.h>
#include <wdf.h>
#include <gpioclx.h>

#define GOYA_GPIO_TAG        'oyoG'
#define GPIO_BANKS           4
#define GPIO_PINS_PER_BANK   32
#define GPIO_TOTAL_PINS      (GPIO_BANKS * GPIO_PINS_PER_BANK)
#define GPIO_REG_SIZE        0x1000

#define GPLR   0x00
#define GPDR   0x0C
#define GPSR   0x18
#define GPCR   0x24
#define GRER   0x30
#define GFER   0x3C
#define GEDR   0x48
#define ED_MASK 0x9C

typedef struct _GOYA_GPIO {
    PUCHAR  Base;                       // mapped controller
    volatile LONG IrqEnabled[GPIO_BANKS];   // pins whose interrupt is enabled (shadow)
    volatile LONG EdgeRise[GPIO_BANKS];
    volatile LONG EdgeFall[GPIO_BANKS];
} GOYA_GPIO, *PGOYA_GPIO;

static __forceinline ULONG BankOff(ULONG Bank)
{
    return ((Bank / 3) << 8) + ((Bank % 3) << 2);
}

static __forceinline ULONG RegRead(PGOYA_GPIO G, ULONG Bank, ULONG Reg)
{
    return READ_REGISTER_ULONG((PULONG)(G->Base + BankOff(Bank) + Reg));
}

static __forceinline VOID RegWrite(PGOYA_GPIO G, ULONG Bank, ULONG Reg, ULONG Value)
{
    WRITE_REGISTER_ULONG((PULONG)(G->Base + BankOff(Bank) + Reg), Value);
}

static __forceinline BOOLEAN ValidBank(ULONG Bank)       { return Bank < GPIO_BANKS; }
static __forceinline BOOLEAN ValidPin(ULONG Bank, ULONG P) { return Bank < GPIO_BANKS && P < GPIO_PINS_PER_BANK; }

/* Latch the shadow edge configuration of one bank into GRER/GFER (only masked bits are touched). */
static VOID ApplyEdges(PGOYA_GPIO G, ULONG Bank)
{
    ULONG mask = (ULONG)G->IrqEnabled[Bank];
    ULONG rise = (RegRead(G, Bank, GRER) & ~mask) | ((ULONG)G->EdgeRise[Bank] & mask);
    ULONG fall = (RegRead(G, Bank, GFER) & ~mask) | ((ULONG)G->EdgeFall[Bank] & mask);
    RegWrite(G, Bank, GRER, rise);
    RegWrite(G, Bank, GFER, fall);
}

/* ---- GpioClx callbacks -------------------------------------------------------------- */

static NTSTATUS GoyaPrepareController(WDFDEVICE Device, PVOID Context, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PGOYA_GPIO g = (PGOYA_GPIO)Context;
    ULONG count = WdfCmResourceListGetCount(Translated);
    ULONG i;
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(Raw);

    for (i = 0; i < count; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = WdfCmResourceListGetDescriptor(Translated, i);
        if (r->Type == CmResourceTypeMemory) {
            if (r->u.Memory.Length < 0x800) {
                return STATUS_DEVICE_CONFIGURATION_ERROR;
            }
            /* MmMapIoSpace works on 8.0 and 10 (MmMapIoSpaceEx is 10 only) */
            g->Base = (PUCHAR)MmMapIoSpace(r->u.Memory.Start, GPIO_REG_SIZE, MmNonCached);
            break;
        }
    }

    return g->Base ? STATUS_SUCCESS : STATUS_DEVICE_CONFIGURATION_ERROR;
}

static NTSTATUS GoyaReleaseController(WDFDEVICE Device, PVOID Context)
{
    PGOYA_GPIO g = (PGOYA_GPIO)Context;
    UNREFERENCED_PARAMETER(Device);
    if (g->Base) {
        MmUnmapIoSpace(g->Base, GPIO_REG_SIZE);
        g->Base = NULL;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS GoyaStartController(PVOID Context, BOOLEAN Restore, WDF_POWER_DEVICE_STATE Prev)
{
    PGOYA_GPIO g = (PGOYA_GPIO)Context;
    ULONG b;
    UNREFERENCED_PARAMETER(Restore);
    UNREFERENCED_PARAMETER(Prev);

    for (b = 0; b < GPIO_BANKS; b++) {
        /* as the vendor driver: clear edge detects and pending status, unmask AP-side edge detect */
        g->IrqEnabled[b] = 0; g->EdgeRise[b] = 0; g->EdgeFall[b] = 0;
        RegWrite(g, b, GRER, 0);
        RegWrite(g, b, GFER, 0);
        RegWrite(g, b, GEDR, 0xFFFFFFFF);
        WRITE_REGISTER_ULONG((PULONG)(g->Base + ED_MASK + BankOff(b)), 0xFFFFFFFF);
    }

    return STATUS_SUCCESS;
}

static NTSTATUS GoyaStopController(PVOID Context, BOOLEAN Save, WDF_POWER_DEVICE_STATE Target)
{
    PGOYA_GPIO g = (PGOYA_GPIO)Context;
    ULONG b;
    UNREFERENCED_PARAMETER(Save);
    UNREFERENCED_PARAMETER(Target);
    for (b = 0; b < GPIO_BANKS; b++) {
        RegWrite(g, b, GRER, 0);
        RegWrite(g, b, GFER, 0);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS GoyaQueryControllerBasicInformation(PVOID Context, PCLIENT_CONTROLLER_BASIC_INFORMATION Info)
{
    UNREFERENCED_PARAMETER(Context);
    Info->Version = GPIO_CONTROLLER_BASIC_INFORMATION_VERSION;
    Info->Size = (USHORT)sizeof(*Info);
    Info->TotalPins = GPIO_TOTAL_PINS;
    Info->NumberOfPinsPerBank = GPIO_PINS_PER_BANK;
    Info->Flags.MemoryMappedController = TRUE;
    Info->Flags.ActiveInterruptsAutoClearOnRead = FALSE;
    Info->Flags.FormatIoRequestsAsMasks = TRUE;
    Info->Flags.DeviceIdlePowerMgmtSupported = FALSE;
    Info->Flags.BankIdlePowerMgmtSupported = FALSE;
    Info->Flags.EmulateDebouncing = TRUE;
    Info->Flags.EmulateActiveBoth = FALSE;      /* handled natively: GRER and GFER both set */
    Info->Flags.IndependentIoHwSupported = TRUE;
    return STATUS_SUCCESS;
}

static NTSTATUS GoyaEnableInterrupt(PVOID Context, PGPIO_ENABLE_INTERRUPT_PARAMETERS P)
{
    PGOYA_GPIO g = (PGOYA_GPIO)Context;
    ULONG mask;
    if (!ValidPin(P->BankId, P->PinNumber)) return STATUS_INVALID_PARAMETER;
    mask = 1UL << P->PinNumber;

    if (P->InterruptMode != Latched) {
        return STATUS_NOT_SUPPORTED;            /* the controller has no level interrupts */
    }

    GPIO_CLX_AcquireInterruptLock(Context, P->BankId);

    InterlockedAnd(&g->EdgeRise[P->BankId], ~(LONG)mask);
    InterlockedAnd(&g->EdgeFall[P->BankId], ~(LONG)mask);
    switch (P->Polarity) {
    case InterruptRisingEdge:  InterlockedOr(&g->EdgeRise[P->BankId], (LONG)mask); break;
    case InterruptFallingEdge: InterlockedOr(&g->EdgeFall[P->BankId], (LONG)mask); break;
    case InterruptActiveBoth:
        InterlockedOr(&g->EdgeRise[P->BankId], (LONG)mask);
        InterlockedOr(&g->EdgeFall[P->BankId], (LONG)mask);
        break;
    default:
        GPIO_CLX_ReleaseInterruptLock(Context, P->BankId);
        return STATUS_NOT_SUPPORTED;
    }

    /* the pin must be an input */
    RegWrite(g, P->BankId, GPDR, RegRead(g, P->BankId, GPDR) & ~mask);
    InterlockedOr(&g->IrqEnabled[P->BankId], (LONG)mask);
    RegWrite(g, P->BankId, GEDR, mask);          /* drop anything stale */
    ApplyEdges(g, P->BankId);

    GPIO_CLX_ReleaseInterruptLock(Context, P->BankId);
    return STATUS_SUCCESS;
}

static NTSTATUS GoyaDisableInterrupt(PVOID Context, PGPIO_DISABLE_INTERRUPT_PARAMETERS P)
{
    PGOYA_GPIO g = (PGOYA_GPIO)Context;
    ULONG mask;
    if (!ValidPin(P->BankId, P->PinNumber)) return STATUS_INVALID_PARAMETER;
    mask = 1UL << P->PinNumber;

    GPIO_CLX_AcquireInterruptLock(Context, P->BankId);
    InterlockedAnd(&g->IrqEnabled[P->BankId], ~(LONG)mask);
    RegWrite(g, P->BankId, GRER, RegRead(g, P->BankId, GRER) & ~mask);
    RegWrite(g, P->BankId, GFER, RegRead(g, P->BankId, GFER) & ~mask);
    RegWrite(g, P->BankId, GEDR, mask);
    GPIO_CLX_ReleaseInterruptLock(Context, P->BankId);
    return STATUS_SUCCESS;
}

static NTSTATUS GoyaMaskInterrupts(PVOID Context, PGPIO_MASK_INTERRUPT_PARAMETERS P)
{
    PGOYA_GPIO g = (PGOYA_GPIO)Context;
    ULONG mask;
    if (!ValidBank(P->BankId)) return STATUS_INVALID_PARAMETER;
    mask = (ULONG)P->PinMask;
    /* masking = stop edge detection for those pins; the shadow keeps the configuration */
    RegWrite(g, P->BankId, GRER, RegRead(g, P->BankId, GRER) & ~mask);
    RegWrite(g, P->BankId, GFER, RegRead(g, P->BankId, GFER) & ~mask);
    InterlockedAnd(&g->IrqEnabled[P->BankId], ~(LONG)mask);
    P->FailedMask = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS GoyaUnmaskInterrupt(PVOID Context, PGPIO_ENABLE_INTERRUPT_PARAMETERS P)
{
    PGOYA_GPIO g = (PGOYA_GPIO)Context;
    ULONG mask;
    if (!ValidPin(P->BankId, P->PinNumber)) return STATUS_INVALID_PARAMETER;
    mask = 1UL << P->PinNumber;
    InterlockedOr(&g->IrqEnabled[P->BankId], (LONG)mask);
    RegWrite(g, P->BankId, GEDR, mask);
    ApplyEdges(g, P->BankId);
    return STATUS_SUCCESS;
}

static NTSTATUS GoyaQueryActiveInterrupts(PVOID Context, PGPIO_QUERY_ACTIVE_INTERRUPTS_PARAMETERS P)
{
    PGOYA_GPIO g = (PGOYA_GPIO)Context;
    if (!ValidBank(P->BankId)) return STATUS_INVALID_PARAMETER;
    P->ActiveMask = (ULONG64)(RegRead(g, P->BankId, GEDR) & (ULONG)g->IrqEnabled[P->BankId]);
    return STATUS_SUCCESS;
}

static NTSTATUS GoyaClearActiveInterrupts(PVOID Context, PGPIO_CLEAR_ACTIVE_INTERRUPTS_PARAMETERS P)
{
    PGOYA_GPIO g = (PGOYA_GPIO)Context;
    if (!ValidBank(P->BankId)) return STATUS_INVALID_PARAMETER;
    RegWrite(g, P->BankId, GEDR, (ULONG)P->ClearActiveMask);   /* write 1 to clear */
    P->FailedClearMask = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS GoyaConnectIoPins(PVOID Context, PGPIO_CONNECT_IO_PINS_PARAMETERS P)
{
    PGOYA_GPIO g = (PGOYA_GPIO)Context;
    ULONG in = 0, out = 0, gpdr;
    USHORT i;
    if (!ValidBank(P->BankId)) return STATUS_INVALID_PARAMETER;
    if (P->ConnectMode != ConnectModeInput && P->ConnectMode != ConnectModeOutput) return STATUS_NOT_SUPPORTED;
    for (i = 0; i < P->PinCount; i++) {
        if (!ValidPin(P->BankId, P->PinNumberTable[i])) return STATUS_INVALID_PARAMETER;
        if (P->ConnectMode == ConnectModeInput) in |= 1UL << P->PinNumberTable[i];
        else out |= 1UL << P->PinNumberTable[i];
    }
    gpdr = RegRead(g, P->BankId, GPDR);
    RegWrite(g, P->BankId, GPDR, (gpdr & ~in) | out);
    return STATUS_SUCCESS;
}

static NTSTATUS GoyaDisconnectIoPins(PVOID Context, PGPIO_DISCONNECT_IO_PINS_PARAMETERS P)
{
    PGOYA_GPIO g = (PGOYA_GPIO)Context;
    ULONG mask = 0;
    USHORT i;
    if (!ValidBank(P->BankId)) return STATUS_INVALID_PARAMETER;
    for (i = 0; i < P->PinCount; i++) {
        if (!ValidPin(P->BankId, P->PinNumberTable[i])) return STATUS_INVALID_PARAMETER;
        mask |= 1UL << P->PinNumberTable[i];
    }
    if (!P->DisconnectFlags.PreserveConfiguration) {
        /* back to input (safe state) unless the pin also carries an interrupt */
        mask &= ~(ULONG)g->IrqEnabled[P->BankId];
        RegWrite(g, P->BankId, GPDR, RegRead(g, P->BankId, GPDR) & ~mask);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS GoyaReadPinsMask(PVOID Context, PGPIO_READ_PINS_MASK_PARAMETERS P)
{
    PGOYA_GPIO g = (PGOYA_GPIO)Context;
    if (!ValidBank(P->BankId)) return STATUS_INVALID_PARAMETER;
    *P->PinValues = RegRead(g, P->BankId, GPLR);     /* level of the pad, input or output */
    return STATUS_SUCCESS;
}

static NTSTATUS GoyaWritePinsMask(PVOID Context, PGPIO_WRITE_PINS_MASK_PARAMETERS P)
{
    PGOYA_GPIO g = (PGOYA_GPIO)Context;
    if (!ValidBank(P->BankId)) return STATUS_INVALID_PARAMETER;
    if (P->SetMask)   RegWrite(g, P->BankId, GPSR, (ULONG)P->SetMask);
    if (P->ClearMask) RegWrite(g, P->BankId, GPCR, (ULONG)P->ClearMask);
    return STATUS_SUCCESS;
}

/* ---- WDF plumbing --------------------------------------------------------------------- */

static NTSTATUS GoyaEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT Init)
{
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES attrs;
    WDFDEVICE device;

    status = GPIO_CLX_ProcessAddDevicePreDeviceCreate(Driver, Init, &attrs);
    if (!NT_SUCCESS(status)) return status;
    status = WdfDeviceCreate(&Init, &attrs, &device);
    if (!NT_SUCCESS(status)) return status;
    return GPIO_CLX_ProcessAddDevicePostDeviceCreate(Driver, device);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    NTSTATUS status;
    WDF_DRIVER_CONFIG cfg;
    WDFDRIVER driver;
    GPIO_CLIENT_REGISTRATION_PACKET pkt;

    WDF_DRIVER_CONFIG_INIT(&cfg, GoyaEvtDeviceAdd);
    cfg.DriverPoolTag = GOYA_GPIO_TAG;
    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &cfg, &driver);
    if (!NT_SUCCESS(status)) return status;

    /* fields are assigned by name so the same source builds against the 8.0 and 10 gpioclx.h */
    RtlZeroMemory(&pkt, sizeof(pkt));
    pkt.Version = GPIO_CLIENT_VERSION;
    pkt.Size = sizeof(pkt);
    pkt.ControllerContextSize = sizeof(GOYA_GPIO);
    pkt.CLIENT_PrepareController = GoyaPrepareController;
    pkt.CLIENT_ReleaseController = GoyaReleaseController;
    pkt.CLIENT_StartController = GoyaStartController;
    pkt.CLIENT_StopController = GoyaStopController;
    pkt.CLIENT_QueryControllerBasicInformation = GoyaQueryControllerBasicInformation;
    pkt.CLIENT_EnableInterrupt = GoyaEnableInterrupt;
    pkt.CLIENT_DisableInterrupt = GoyaDisableInterrupt;
    pkt.CLIENT_UnmaskInterrupt = GoyaUnmaskInterrupt;
    pkt.CLIENT_MaskInterrupts = GoyaMaskInterrupts;
    pkt.CLIENT_QueryActiveInterrupts = GoyaQueryActiveInterrupts;
    pkt.CLIENT_ClearActiveInterrupts = GoyaClearActiveInterrupts;
    pkt.CLIENT_ConnectIoPins = GoyaConnectIoPins;
    pkt.CLIENT_DisconnectIoPins = GoyaDisconnectIoPins;
    pkt.CLIENT_ReadGpioPinsUsingMask = GoyaReadPinsMask;
    pkt.CLIENT_WriteGpioPinsUsingMask = GoyaWritePinsMask;

    return GPIO_CLX_RegisterClient(driver, &pkt, RegistryPath);
}
