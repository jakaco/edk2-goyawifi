/*
 * goyasdhc - Windows 8.0 RT SD host miniport (sdport) for the Marvell PXA986/PXA988
 * (PXAv3, SDHCI 3.0 compatible) controller of the Galaxy Tab 3 Lite. Slot 0 = SDH1 = microSD.
 *
 * Design:
 *  - Standard SDHCI register model, PIO data transfers only (robust first; no ADMA).
 *  - Same board quirks as the EDK2 driver (Pxa988SdhciDxe):
 *      * base clock is 52 MHz (APMU SDH1 clock gate/divider is left as the firmware set it),
 *      * no 1.8 V signalling, no UHS, no tuning,
 *      * card detect is GPIO11 (active low), fed to the controller through the
 *        CD_SIG_SEL/CD_TEST_LVL override bits in Host Control 1,
 *      * the slot supply (88PM822 LDO4/LDO5) was powered by firmware; Windows leaves it on.
 *  - Boot capable: StartType 0 in the INF, crash dump supported (no WPP/KMDF).
 */
#include <ntddk.h>
#include <sdport.h>

#define GOYA_SD_BASE_CLOCK_HZ   52000000UL
#define GOYA_GPIO_PHYS          0xD4019000ULL
#define GOYA_GPIO_LEN           0x100
#define GOYA_SD_CD_GPIO         11          /* bank 0, active low */

/* SDHCI registers */
#define R_BLKSIZE       0x04    /* 16 */
#define R_BLKCOUNT      0x06    /* 16 */
#define R_ARG           0x08    /* 32 */
#define R_XFER          0x0C    /* 16 */
#define R_CMD           0x0E    /* 16 */
#define R_RESP0         0x10
#define R_BUFFER        0x20    /* 32 */
#define R_PRESENT       0x24    /* 32 */
#define R_HOSTCTRL      0x28    /* 8  */
#define R_POWER         0x29    /* 8  */
#define R_CLKCTRL       0x2C    /* 16 */
#define R_TIMEOUT       0x2E    /* 8  */
#define R_SWRESET       0x2F    /* 8  */
#define R_INTSTS        0x30    /* 32 */
#define R_INTEN         0x34    /* 32 */
#define R_SIGEN         0x38    /* 32 */
#define R_CAPS          0x40    /* 32 */

#define PRES_CMD_INHIBIT   0x00000001
#define PRES_DAT_INHIBIT   0x00000002
#define PRES_DAT_ACTIVE    0x00000004

#define HC_4BIT            0x02
#define HC_HISPEED         0x04
#define HC_8BIT            0x20
#define HC_CD_TEST_LVL     0x40
#define HC_CD_SIG_SEL      0x80

#define INT_CC             0x00000001
#define INT_TC             0x00000002
#define INT_BGE            0x00000004
#define INT_DMA            0x00000008
#define INT_BWR            0x00000010
#define INT_BRR            0x00000020
#define INT_CINS           0x00000040
#define INT_CRM            0x00000080
#define INT_CINT           0x00000100
#define INT_RETUNE         0x00001000
#define INT_ERR            0x00008000
#define INT_ERR_ALL        0x03FF0000
#define INT_EVT_MASK       0x00007FFF

#define ERR_CTO   0x00010000
#define ERR_CCRC  0x00020000
#define ERR_CEND  0x00040000
#define ERR_CIDX  0x00080000
#define ERR_DTO   0x00100000
#define ERR_DCRC  0x00200000
#define ERR_DEND  0x00400000

typedef struct _GOYA_SDHC {
    volatile UCHAR       *Regs;
    BOOLEAN              Crashdump;
    volatile LONG        Remaining;       /* bytes still to move in the current data phase */
    ULONG                BlockSize;
    SDPORT_CAPABILITIES  Caps;
} GOYA_SDHC;

static volatile ULONG *gGpio;

/* ---- register helpers ---- */
static __forceinline ULONG  Rd32(GOYA_SDHC *H, ULONG o) { return READ_REGISTER_ULONG((volatile ULONG *)(H->Regs + o)); }
static __forceinline USHORT Rd16(GOYA_SDHC *H, ULONG o) { return READ_REGISTER_USHORT((volatile USHORT *)(H->Regs + o)); }
static __forceinline UCHAR  Rd8 (GOYA_SDHC *H, ULONG o) { return READ_REGISTER_UCHAR(H->Regs + o); }
static __forceinline VOID   Wr32(GOYA_SDHC *H, ULONG o, ULONG v)  { WRITE_REGISTER_ULONG((volatile ULONG *)(H->Regs + o), v); }
static __forceinline VOID   Wr16(GOYA_SDHC *H, ULONG o, USHORT v) { WRITE_REGISTER_USHORT((volatile USHORT *)(H->Regs + o), v); }
static __forceinline VOID   Wr8 (GOYA_SDHC *H, ULONG o, UCHAR v)  { WRITE_REGISTER_UCHAR(H->Regs + o, v); }

static BOOLEAN CardPresentGpio(VOID)
{
    if (gGpio == NULL) {
        return TRUE;                              /* unknown (crash dump): assume inserted */
    }
    return (READ_REGISTER_ULONG(gGpio) & (1UL << GOYA_SD_CD_GPIO)) == 0;
}

static VOID ForceCardDetect(GOYA_SDHC *H)
{
    UCHAR c = Rd8(H, R_HOSTCTRL) | HC_CD_SIG_SEL;
    if (CardPresentGpio()) c |= HC_CD_TEST_LVL; else c &= (UCHAR)~HC_CD_TEST_LVL;
    Wr8(H, R_HOSTCTRL, c);
}

static VOID IntEnable(GOYA_SDHC *H, ULONG mask)
{
    ULONG v = Rd32(H, R_INTEN) | mask;
    Wr32(H, R_INTEN, v);
    if (!H->Crashdump) Wr32(H, R_SIGEN, v);
}

static VOID IntDisable(GOYA_SDHC *H, ULONG mask)
{
    ULONG v = Rd32(H, R_INTEN) & ~mask;
    Wr32(H, R_INTEN, v);
    Wr32(H, R_SIGEN, v);
}

/* sdport "standard events" are the SDHCI Normal(15:0)/Error(31:16) interrupt status bits */
static ULONG StdEventsToMask(ULONG ev)  { return (ev & INT_EVT_MASK) | INT_ERR_ALL; }

static NTSTATUS ErrorToStatus(ULONG e)
{
    if (e == 0) return STATUS_SUCCESS;
    if (e & (ERR_CTO | ERR_DTO))   return STATUS_IO_TIMEOUT;
    if (e & (ERR_CCRC | ERR_DCRC)) return STATUS_CRC_ERROR;
    if (e & (ERR_CEND | ERR_DEND)) return STATUS_DEVICE_DATA_ERROR;
    if (e & ERR_CIDX)              return STATUS_DEVICE_PROTOCOL_ERROR;
    return STATUS_IO_DEVICE_ERROR;
}

/* ---- sdport callbacks ---- */

static NTSTATUS GoyaGetSlotCount(SD_MINIPORT *Miniport, UCHAR *SlotCount)
{
    if (Miniport->ConfigurationInfo.BusType != SdBusTypeAcpi) return STATUS_NOT_SUPPORTED;
    *SlotCount = 1;
    return STATUS_SUCCESS;
}

static VOID GoyaGetCapabilities(PVOID Ext, SDPORT_CAPABILITIES *Caps)
{
    RtlCopyMemory(Caps, &((GOYA_SDHC *)Ext)->Caps, sizeof(*Caps));
}

static NTSTATUS GoyaInitialize(PVOID Ext, PHYSICAL_ADDRESS Phys, PVOID Virt, ULONG Length, BOOLEAN Crashdump)
{
    GOYA_SDHC *H = (GOYA_SDHC *)Ext;
    SDPORT_CAPABILITIES *c = &H->Caps;

    UNREFERENCED_PARAMETER(Phys);
    UNREFERENCED_PARAMETER(Length);
    H->Regs = (volatile UCHAR *)Virt;
    H->Crashdump = Crashdump;

    RtlZeroMemory(c, sizeof(*c));
    c->SpecVersion = 3;
    c->MaximumOutstandingRequests = 1;
    c->MaximumBlockSize = 512;
    c->MaximumBlockCount = 0xFFFF;
    c->BaseClockFrequencyKhz = GOYA_SD_BASE_CLOCK_HZ / 1000;
    c->DmaDescriptorSize = 0;
    c->AlignmentRequirement = sizeof(ULONG) - 1;
    c->Flags.UsePioForRead = 1;
    c->Flags.UsePioForWrite = 1;
    c->Supported.ScatterGatherDma = 0;
    c->Supported.HighSpeed = 1;
    c->Supported.BusWidth8Bit = 0;                /* microSD slot: 4 data lines */
    c->Supported.SignalingVoltage18V = 0;         /* no 1.8 V on this board */
    c->Supported.Voltage33V = 1;
    c->Supported.Voltage30V = 1;
    c->Supported.DriverTypeA = 1;
    c->Supported.DriverTypeB = 1;
    c->Supported.DriverTypeC = 1;
    c->Supported.DriverTypeD = 1;
    c->Supported.Limit200mA = 1;
    c->Supported.Limit400mA = 1;
    c->Supported.Limit600mA = 1;
    c->Supported.Limit800mA = 1;
    c->Supported.AutoCmd12 = 1;

    ForceCardDetect(H);
    return STATUS_SUCCESS;
}

static NTSTATUS ResetHost(GOYA_SDHC *H, SDPORT_RESET_TYPE Type)
{
    UCHAR mask, v;
    ULONG retries = 1000;

    switch (Type) {
    case SdResetTypeAll: mask = 0x01; break;
    case SdResetTypeCmd: mask = 0x02; break;
    case SdResetTypeDat: mask = 0x04; break;
    default: return STATUS_INVALID_PARAMETER;
    }

    Wr8(H, R_SWRESET, mask);
    do {
        SdPortWait(100);
        v = Rd8(H, R_SWRESET);
    } while ((v & mask) && --retries);
    if (v & mask) return STATUS_IO_TIMEOUT;

    if (Type == SdResetTypeAll) {
        Wr32(H, R_INTEN, 0);
        Wr32(H, R_SIGEN, 0);
        Wr32(H, R_INTSTS, 0xFFFFFFFF);
        Wr8(H, R_TIMEOUT, 0x0E);                  /* longest data timeout */
        Wr8(H, R_POWER, 0x0F);                    /* 3.3 V, bus power on (LDOs are firmware-enabled) */
        ForceCardDetect(H);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS SetClock(GOYA_SDHC *H, ULONG Khz)
{
    ULONG target = Khz * 1000, n, retries = 1000;
    USHORT clk;

    if (target == 0) target = 400000;
    if (target >= GOYA_SD_BASE_CLOCK_HZ) {
        n = 0;
    } else {
        n = (GOYA_SD_BASE_CLOCK_HZ + 2 * target - 1) / (2 * target);
        if (n > 0x3FF) n = 0x3FF;
    }

    Wr16(H, R_CLKCTRL, 0);                        /* stop SDCLK */
    clk = (USHORT)(((n & 0xFF) << 8) | (((n >> 8) & 3) << 6) | 0x0001);
    Wr16(H, R_CLKCTRL, clk);
    while (!(Rd16(H, R_CLKCTRL) & 0x0002) && --retries) SdPortWait(100);
    if (!(Rd16(H, R_CLKCTRL) & 0x0002)) return STATUS_IO_TIMEOUT;
    Wr16(H, R_CLKCTRL, (USHORT)(clk | 0x0004));   /* SDCLK on */
    return STATUS_SUCCESS;
}

static NTSTATUS GoyaBusOperation(PVOID Ext, SDPORT_BUS_OPERATION *Op)
{
    GOYA_SDHC *H = (GOYA_SDHC *)Ext;
    UCHAR c;

    switch (Op->Type) {
    case SdResetHost:
        return ResetHost(H, Op->Parameters.ResetType);
    case SdSetClock:
        return SetClock(H, Op->Parameters.FrequencyKhz);
    case SdSetBusWidth:
        c = Rd8(H, R_HOSTCTRL) & (UCHAR)~(HC_4BIT | HC_8BIT);
        if (Op->Parameters.BusWidth == SdBusWidth4Bit) c |= HC_4BIT;
        else if (Op->Parameters.BusWidth == SdBusWidth8Bit) c |= HC_8BIT;
        Wr8(H, R_HOSTCTRL, c);
        return STATUS_SUCCESS;
    case SdSetBusSpeed:
        c = Rd8(H, R_HOSTCTRL);
        if (Op->Parameters.BusSpeed == SdBusSpeedNormal)      c &= (UCHAR)~HC_HISPEED;
        else if (Op->Parameters.BusSpeed == SdBusSpeedHigh)   c |= HC_HISPEED;
        else return STATUS_NOT_SUPPORTED;
        Wr8(H, R_HOSTCTRL, c);
        return STATUS_SUCCESS;
    case SdSetSignalingVoltage:
        return (Op->Parameters.SignalingVoltage == SdSignalingVoltage18) ? STATUS_NOT_SUPPORTED : STATUS_SUCCESS;
    case SdSetVoltage:                            /* fixed 3.3 V rail */
    case SdResetHw:
    case SdSetDriveStrength:
    case SdSetDriverType:
    case SdSetPresetValue:
    case SdSetBlockGapInterrupt:
        return STATUS_SUCCESS;
    case SdExecuteTuning:
        return STATUS_NOT_SUPPORTED;
    default:
        return STATUS_INVALID_PARAMETER;
    }
}

static BOOLEAN GoyaGetCardDetect(PVOID Ext)
{
    GOYA_SDHC *H = (GOYA_SDHC *)Ext;
    BOOLEAN p = CardPresentGpio();
    UCHAR c = Rd8(H, R_HOSTCTRL) | HC_CD_SIG_SEL;
    if (p) c |= HC_CD_TEST_LVL; else c &= (UCHAR)~HC_CD_TEST_LVL;
    Wr8(H, R_HOSTCTRL, c);
    return p;
}

static BOOLEAN GoyaGetWriteProtect(PVOID Ext)
{
    UNREFERENCED_PARAMETER(Ext);
    return FALSE;                                 /* microSD slot has no WP switch */
}

static BOOLEAN GoyaInterrupt(PVOID Ext, PULONG Events, PULONG Errors, PBOOLEAN CardChange,
                             PBOOLEAN SdioInterrupt, PBOOLEAN Tuning)
{
    GOYA_SDHC *H = (GOYA_SDHC *)Ext;
    ULONG st = Rd32(H, R_INTSTS), ev;

    UNREFERENCED_PARAMETER(CardChange);
    UNREFERENCED_PARAMETER(SdioInterrupt);
    UNREFERENCED_PARAMETER(Tuning);

    if (st == 0) return FALSE;

    if (st & (INT_BRR | INT_BWR)) {               /* re-armed by the next StartTransfer */
        IntDisable(H, st & (INT_BRR | INT_BWR));
    }
    Wr32(H, R_INTSTS, st);

    st &= ~(INT_CINS | INT_CRM | INT_CINT | INT_RETUNE);
    ev = st & INT_EVT_MASK;
    if (st & INT_ERR_ALL) ev |= INT_ERR;
    *Events = ev;
    *Errors = (st & INT_ERR_ALL) >> 16;
    return (ev != 0) || (*Errors != 0);
}

static NTSTATUS SendCommand(GOYA_SDHC *H, SDPORT_REQUEST *Req)
{
    SDPORT_COMMAND *cmd = &Req->Command;
    USHORT xfer = 0, c = 0;
    ULONG required = INT_CC, wait = 0, inhibit = PRES_CMD_INHIBIT;

    if (cmd->TransferType != SdTransferTypeNone && cmd->TransferType != SdTransferTypeUndefined) {
        inhibit |= PRES_DAT_INHIBIT;
    }
    if (cmd->ResponseType == SdResponseTypeR1B || cmd->ResponseType == SdResponseTypeR5B) {
        inhibit |= PRES_DAT_INHIBIT;
    }
    if (cmd->Type == SdCommandTypeAbort) inhibit &= ~PRES_DAT_INHIBIT;

    while ((Rd32(H, R_PRESENT) & inhibit) && wait < 100000) { SdPortWait(10); wait += 10; }
    if (Rd32(H, R_PRESENT) & inhibit) return STATUS_DEVICE_BUSY;

    if (cmd->TransferType != SdTransferTypeNone && cmd->TransferType != SdTransferTypeUndefined) {
        H->BlockSize = cmd->BlockSize;
        Wr16(H, R_BLKSIZE, (USHORT)(0x7000 | (cmd->BlockSize & 0xFFF)));
        Wr16(H, R_BLKCOUNT, (USHORT)cmd->BlockCount);
        if (cmd->BlockCount > 1) {
            xfer |= 0x0020 | 0x0002;              /* multi block, block count enable */
            if (cmd->TransferType != SdTransferTypeMultiBlockNoStop) xfer |= 0x0004; /* Auto CMD12 */
        }
        if (cmd->TransferDirection == SdTransferDirectionRead) xfer |= 0x0010;
        c |= 0x0020;                              /* data present */
        if (cmd->TransferDirection == SdTransferDirectionRead) required |= INT_BRR;
        else required |= INT_BWR;
        IntEnable(H, required & (INT_BRR | INT_BWR));
    } else {
        cmd->BlockCount = 0;
        cmd->Length = 0;
    }
    InterlockedExchange(&H->Remaining, (LONG)cmd->Length);

    switch (cmd->ResponseType) {
    case SdResponseTypeNone: break;
    case SdResponseTypeR1: case SdResponseTypeR5: case SdResponseTypeR6:
        c |= 0x0002 | 0x0008 | 0x0010; break;     /* 48 bit, CRC + index check */
    case SdResponseTypeR1B: case SdResponseTypeR5B:
        c |= 0x0003 | 0x0008 | 0x0010; break;     /* 48 bit + busy */
    case SdResponseTypeR2:
        c |= 0x0001 | 0x0008; break;              /* 136 bit */
    case SdResponseTypeR3: case SdResponseTypeR4:
        c |= 0x0002; break;
    default:
        return STATUS_INVALID_PARAMETER;
    }
    switch (cmd->Type) {
    case SdCommandTypeSuspend: c |= 0x0040; break;
    case SdCommandTypeResume:  c |= 0x0080; break;
    case SdCommandTypeAbort:   c |= 0x00C0; break;
    default: break;
    }
    c |= (USHORT)(cmd->Index << 8);

    Req->RequiredEvents = required;
    Wr32(H, R_ARG, cmd->Argument);
    Wr16(H, R_XFER, xfer);
    Wr16(H, R_CMD, c);
    return STATUS_PENDING;
}

static NTSTATUS StartTransfer(GOYA_SDHC *H, SDPORT_REQUEST *Req)
{
    SDPORT_COMMAND *cmd = &Req->Command;
    ULONG chunk = (ULONG)H->Remaining, words, i, need;
    PULONG buf = (PULONG)cmd->DataBuffer;

    if (chunk > H->BlockSize) chunk = H->BlockSize;
    words = chunk / sizeof(ULONG);
    if (cmd->TransferDirection == SdTransferDirectionRead) {
        for (i = 0; i < words; i++) buf[i] = READ_REGISTER_ULONG((volatile ULONG *)(H->Regs + R_BUFFER));
    } else {
        for (i = 0; i < words; i++) WRITE_REGISTER_ULONG((volatile ULONG *)(H->Regs + R_BUFFER), buf[i]);
    }
    InterlockedExchangeAdd(&H->Remaining, -(LONG)chunk);

    if (H->Remaining > 0) {
        cmd->DataBuffer += chunk;
        need = (cmd->TransferDirection == SdTransferDirectionRead) ? INT_BRR : INT_BWR;
        Req->Status = STATUS_MORE_PROCESSING_REQUIRED;
        IntEnable(H, need);
    } else {
        need = INT_TC;
        Req->Status = STATUS_SUCCESS;
    }
    Req->RequiredEvents = need;
    return STATUS_PENDING;
}

static NTSTATUS GoyaIssueRequest(PVOID Ext, SDPORT_REQUEST *Req)
{
    GOYA_SDHC *H = (GOYA_SDHC *)Ext;
    Req->Status = STATUS_IO_DEVICE_ERROR;
    switch (Req->Type) {
    case SdRequestTypeCommandNoTransfer:
    case SdRequestTypeCommandWithTransfer:
        return SendCommand(H, Req);
    case SdRequestTypeStartTransfer:
        return StartTransfer(H, Req);
    default:
        return STATUS_NOT_SUPPORTED;
    }
}

static VOID GoyaGetResponse(PVOID Ext, SDPORT_COMMAND *Cmd, PVOID Out)
{
    GOYA_SDHC *H = (GOYA_SDHC *)Ext;
    PULONG r = (PULONG)Out;

    switch (Cmd->ResponseType) {
    case SdResponseTypeNone:
        break;
    case SdResponseTypeR2:
        r[0] = Rd32(H, R_RESP0);      r[1] = Rd32(H, R_RESP0 + 4);
        r[2] = Rd32(H, R_RESP0 + 8);  r[3] = Rd32(H, R_RESP0 + 12);
        break;
    default:
        r[0] = Rd32(H, R_RESP0);
        break;
    }
}

static VOID GoyaToggleEvents(PVOID Ext, ULONG Mask, BOOLEAN Enable)
{
    GOYA_SDHC *H = (GOYA_SDHC *)Ext;
    ULONG m = StdEventsToMask(Mask);
    if (Enable) IntEnable(H, m); else IntDisable(H, m);
}

static VOID GoyaClearEvents(PVOID Ext, ULONG Mask)
{
    Wr32((GOYA_SDHC *)Ext, R_INTSTS, StdEventsToMask(Mask));
}

static VOID GoyaRequestDpc(PVOID Ext, SDPORT_REQUEST *Req, ULONG Events, ULONG Errors)
{
    GOYA_SDHC *H = (GOYA_SDHC *)Ext;

    Req->RequiredEvents &= ~Events;
    if (Errors) {
        Req->RequiredEvents = 0;
        Req->Status = ErrorToStatus(Errors << 16);
        SdPortCompleteRequest(Req, Req->Status);
    } else if (Req->RequiredEvents == 0) {
        if (Req->Status != STATUS_MORE_PROCESSING_REQUIRED) {
            Req->Status = STATUS_SUCCESS;
            if (Req->Command.ResponseType == SdResponseTypeR1B ||
                Req->Command.ResponseType == SdResponseTypeR5B) {
                ULONG retries = 1000;
                while ((Rd32(H, R_PRESENT) & PRES_DAT_ACTIVE) && retries--) SdPortWait(100);
                if (Rd32(H, R_PRESENT) & PRES_DAT_ACTIVE) Req->Status = STATUS_IO_TIMEOUT;
            }
        }
        SdPortCompleteRequest(Req, Req->Status);
    }
}

static VOID GoyaSaveContext(PVOID Ext)    { UNREFERENCED_PARAMETER(Ext); }
static VOID GoyaRestoreContext(PVOID Ext) { ForceCardDetect((GOYA_SDHC *)Ext); }
static VOID GoyaCleanup(SD_MINIPORT *M)   { UNREFERENCED_PARAMETER(M); }

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    SDPORT_INITIALIZATION_DATA init;
    PHYSICAL_ADDRESS gpio;

    if (KeGetCurrentIrql() < DISPATCH_LEVEL && gGpio == NULL) {
        gpio.QuadPart = GOYA_GPIO_PHYS;
        gGpio = (volatile ULONG *)MmMapIoSpace(gpio, GOYA_GPIO_LEN, MmNonCached);
    }

    RtlZeroMemory(&init, sizeof(init));
    init.StructureSize = sizeof(init);
    init.GetSlotCount = GoyaGetSlotCount;
    init.GetSlotCapabilities = GoyaGetCapabilities;
    init.Initialize = GoyaInitialize;
    init.IssueBusOperation = GoyaBusOperation;
    init.GetCardDetectState = GoyaGetCardDetect;
    init.GetWriteProtectState = GoyaGetWriteProtect;
    init.Interrupt = GoyaInterrupt;
    init.IssueRequest = GoyaIssueRequest;
    init.GetResponse = GoyaGetResponse;
    init.ToggleEvents = GoyaToggleEvents;
    init.ClearEvents = GoyaClearEvents;
    init.RequestDpc = GoyaRequestDpc;
    init.SaveContext = GoyaSaveContext;
    init.RestoreContext = GoyaRestoreContext;
    init.PowerControlCallback = NULL;
    init.Cleanup = GoyaCleanup;
    init.PrivateExtensionSize = sizeof(GOYA_SDHC);
    init.CrashdumpSupported = TRUE;

    return SdPortInitialize(DriverObject, RegistryPath, &init);
}
