/* Marvell PXA986/PXA988 TWSI (PXA I2C) controller - SpbCx client driver (Windows 8.0 RT / Windows 10).

   Ported from drivers/i2c/busses/i2c-pxa.c of the Samsung SM-T110 (goyawifi) 4.19 kernel, using the
   structure of Microsoft's imx-iotcore imxi2c SpbCx driver (MIT). Register sequences are the ones the
   firmware's Pm822Lib already uses on the real hardware (PMIC reads/writes on TWSI2 work).

   Design: polled master-mode PIO. Every SpbCx request (read, write, sequence) is carried out
   synchronously inside its callback; no interrupt is used (the ACPI node lists none). Transfers on this
   device are tiny (touch report, sensor registers), so the busy-wait is short.

   ACPI resources of the controller device, in order:
     1. Memory: the TWSI register block (4 KB, e.g. TWSI1 0xD4010800)
     2. Memory (optional, 4 bytes): the APBC clock register of this TWSI (e.g. 0xD4015060), so the driver
        can make sure clock + reset are right. If absent, the clock is left as the firmware set it.
*/
#include <ntddk.h>
#include <wdf.h>
#include <spb.h>
#include <spbcx.h>
#include <reshub.h>

#define GOYA_I2C_TAG 'c2oG'

/* PXA I2C registers */
#define I2C_IBMR   0x00
#define I2C_IDBR   0x08
#define I2C_ICR    0x10
#define I2C_ISR    0x18
#define I2C_ISAR   0x20

#define ICR_START  (1UL << 0)
#define ICR_STOP   (1UL << 1)
#define ICR_ACKNAK (1UL << 2)
#define ICR_TB     (1UL << 3)
#define ICR_MA     (1UL << 4)
#define ICR_SCLE   (1UL << 5)
#define ICR_IUE    (1UL << 6)
#define ICR_GCD    (1UL << 7)
#define ICR_UR     (1UL << 14)
#define ICR_FM     (1UL << 15)

#define ISR_RWM    (1UL << 0)
#define ISR_ACKNAK (1UL << 1)
#define ISR_UB     (1UL << 2)
#define ISR_IBB    (1UL << 3)
#define ISR_ALD    (1UL << 5)
#define ISR_ITE    (1UL << 6)
#define ISR_IRF    (1UL << 7)
#define ISR_BED    (1UL << 10)

#define APBC_APBCLK  (1UL << 0)
#define APBC_FNCLK   (1UL << 1)
#define APBC_RST     (1UL << 2)

#define I2C_MAX_ADDRESS  0x7F
#define I2C_POLL_US      10
#define I2C_POLL_LOOPS   3000          /* 30 ms per byte, far more than any slave clock stretch we expect */

typedef struct _GOYA_I2C {
    WDFDEVICE Device;
    PUCHAR    Regs;                    /* mapped TWSI block */
    PULONG    ClockReg;                /* mapped APBC register, may be NULL */
    BOOLEAN   FastMode;
} GOYA_I2C, *PGOYA_I2C;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(GOYA_I2C, GetI2c)

typedef struct _GOYA_TARGET {
    USHORT  Address;                   /* 7-bit */
    ULONG   Speed;
} GOYA_TARGET, *PGOYA_TARGET;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(GOYA_TARGET, GetTarget)

#pragma pack(push, 1)
typedef struct _GOYA_I2C_DESCRIPTOR {   /* ACPI 5.0 table 6-192 */
    PNP_SERIAL_BUS_DESCRIPTOR SerialBusDescriptor;
    ULONG  ConnectionSpeed;
    USHORT SlaveAddress;
} GOYA_I2C_DESCRIPTOR, *PGOYA_I2C_DESCRIPTOR;
#pragma pack(pop)

#define I2C_SERIAL_BUS_TYPE 0x01
#define I2C_TYPE_FLAG_10BIT 0x0001
#define I2C_GENERAL_FLAG_SLAVE 0x01

static __forceinline ULONG Rd(PGOYA_I2C I, ULONG R) { return READ_REGISTER_ULONG((PULONG)(I->Regs + R)); }
static __forceinline VOID  Wr(PGOYA_I2C I, ULONG R, ULONG V) { WRITE_REGISTER_ULONG((PULONG)(I->Regs + R), V); }

/* ---- controller helpers ------------------------------------------------------------ */

static VOID I2cResetUnit(PGOYA_I2C I)
{
    /* i2c_pxa_reset(): unit reset, clear status, set control (polled: no interrupt enables), enable */
    Wr(I, I2C_ICR, ICR_UR);
    Wr(I, I2C_ISR, 0x7FF);
    Wr(I, I2C_ICR, 0);
    Wr(I, I2C_ICR, ICR_GCD | ICR_SCLE | (I->FastMode ? ICR_FM : 0));
    Wr(I, I2C_ICR, Rd(I, I2C_ICR) | ICR_IUE);
    KeStallExecutionProcessor(100);
}

static VOID I2cEnsureClock(PGOYA_I2C I)
{
    ULONG v;
    if (I->ClockReg == NULL) return;
    v = READ_REGISTER_ULONG(I->ClockReg);
    if ((v & (APBC_APBCLK | APBC_FNCLK | APBC_RST)) != (APBC_APBCLK | APBC_FNCLK)) {
        WRITE_REGISTER_ULONG(I->ClockReg, (v & ~APBC_RST) | APBC_APBCLK | APBC_FNCLK);
        KeStallExecutionProcessor(10);
    }
}

static NTSTATUS I2cWaitIsr(PGOYA_I2C I, ULONG Bit)
{
    ULONG n;
    for (n = 0; n < I2C_POLL_LOOPS; n++) {
        ULONG isr = Rd(I, I2C_ISR);
        if (isr & Bit) {
            Wr(I, I2C_ISR, Bit);                     /* write 1 to clear */
            if (isr & ISR_ALD) { Wr(I, I2C_ISR, ISR_ALD); return STATUS_IO_DEVICE_ERROR; }   /* lost arbitration */
            return STATUS_SUCCESS;
        }
        if (isr & (ISR_BED | ISR_ALD)) {
            Wr(I, I2C_ISR, ISR_BED | ISR_ALD);
            return STATUS_IO_DEVICE_ERROR;
        }
        KeStallExecutionProcessor(I2C_POLL_US);
    }
    return STATUS_IO_TIMEOUT;
}

/* Send one byte (address or data). Flags = ICR_START and/or ICR_STOP. Returns STATUS_NO_SUCH_DEVICE on NAK. */
static NTSTATUS I2cTxByte(PGOYA_I2C I, UCHAR Data, ULONG Flags)
{
    NTSTATUS st;
    Wr(I, I2C_IDBR, Data);
    Wr(I, I2C_ICR, (Rd(I, I2C_ICR) & ~(ICR_START | ICR_STOP | ICR_ACKNAK | ICR_TB)) | Flags | ICR_TB | ICR_IUE | ICR_SCLE);
    st = I2cWaitIsr(I, ISR_ITE);
    if (NT_SUCCESS(st) && (Rd(I, I2C_ISR) & ISR_ACKNAK)) {
        st = STATUS_NO_SUCH_DEVICE;                  /* slave did not ACK */
    }
    return st;
}

static NTSTATUS I2cRxByte(PGOYA_I2C I, UCHAR *Data, BOOLEAN Last, BOOLEAN Stop)
{
    NTSTATUS st;
    ULONG f = ICR_TB | ICR_IUE | ICR_SCLE;
    if (Last) f |= ICR_ACKNAK;                       /* NAK the last byte */
    if (Last && Stop) f |= ICR_STOP;
    Wr(I, I2C_ICR, (Rd(I, I2C_ICR) & ~(ICR_START | ICR_STOP | ICR_ACKNAK | ICR_TB)) | f);
    st = I2cWaitIsr(I, ISR_IRF);
    if (NT_SUCCESS(st)) *Data = (UCHAR)Rd(I, I2C_IDBR);
    return st;
}

static VOID I2cRecover(PGOYA_I2C I)
{
    /* after an error: STOP condition, then reset the unit so the next request starts clean */
    Wr(I, I2C_ICR, (Rd(I, I2C_ICR) & ~(ICR_START | ICR_TB | ICR_ACKNAK)) | ICR_STOP | ICR_MA);
    KeStallExecutionProcessor(50);
    I2cResetUnit(I);
}

/* ---- MDL chain access -------------------------------------------------------------- */

static PUCHAR MdlByte(PMDL Mdl, size_t Index)
{
    while (Mdl != NULL) {
        size_t n = MmGetMdlByteCount(Mdl);
        if (Index < n) {
            PUCHAR p = (PUCHAR)MmGetSystemAddressForMdlSafe(Mdl, NormalPagePriority);
            return p ? p + Index : NULL;
        }
        Index -= n;
        Mdl = Mdl->Next;
    }
    return NULL;
}

/* ---- one transfer descriptor ------------------------------------------------------- */

/* Writes (Dir == write) or reads Length bytes. Start: issue (repeated) START with the address.
   Stop: issue STOP with the last byte of this transfer. */
static NTSTATUS I2cTransfer(PGOYA_I2C I, UCHAR Addr7, BOOLEAN Read, PMDL Mdl, size_t Length, BOOLEAN Stop, size_t *Done)
{
    NTSTATUS st;
    size_t i;
    *Done = 0;

    if (Length == 0) {
        /* address-only probe (SpbCx "bufferless" write) */
        return I2cTxByte(I, (UCHAR)(Addr7 << 1) | (Read ? 1 : 0), ICR_START | (Stop ? ICR_STOP : 0));
    }

    st = I2cTxByte(I, (UCHAR)((Addr7 << 1) | (Read ? 1 : 0)), ICR_START);
    if (!NT_SUCCESS(st)) return st;

    for (i = 0; i < Length; i++) {
        PUCHAR p = MdlByte(Mdl, i);
        if (p == NULL) return STATUS_INSUFFICIENT_RESOURCES;
        if (Read) {
            st = I2cRxByte(I, p, i == Length - 1, Stop);
        } else {
            st = I2cTxByte(I, *p, (Stop && i == Length - 1) ? ICR_STOP : 0);
        }
        if (!NT_SUCCESS(st)) return st;
        (*Done)++;
    }
    return STATUS_SUCCESS;
}

/* ---- SpbCx callbacks --------------------------------------------------------------- */

static NTSTATUS OnTargetConnect(WDFDEVICE Controller, SPBTARGET Target)
{
    SPB_CONNECTION_PARAMETERS params;
    RH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER *conn;
    PNP_SERIAL_BUS_DESCRIPTOR *bus;
    PGOYA_I2C_DESCRIPTOR d;
    PGOYA_TARGET t = GetTarget(Target);
    PGOYA_I2C I = GetI2c(Controller);
    UNREFERENCED_PARAMETER(I);

    SPB_CONNECTION_PARAMETERS_INIT(&params);
    SpbTargetGetConnectionParameters(Target, &params);
    conn = (RH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER *)params.ConnectionParameters;
    if (conn == NULL || conn->PropertiesLength < sizeof(PNP_SERIAL_BUS_DESCRIPTOR)) return STATUS_INVALID_PARAMETER;

    bus = (PNP_SERIAL_BUS_DESCRIPTOR *)conn->ConnectionProperties;
    if (bus->SerialBusType != I2C_SERIAL_BUS_TYPE) return STATUS_NOT_SUPPORTED;
    if (conn->PropertiesLength < sizeof(GOYA_I2C_DESCRIPTOR)) return STATUS_INVALID_PARAMETER;

    d = (PGOYA_I2C_DESCRIPTOR)conn->ConnectionProperties;
    if (d->SerialBusDescriptor.GeneralFlags & I2C_GENERAL_FLAG_SLAVE) return STATUS_NOT_SUPPORTED;      /* master only */
    if (d->SerialBusDescriptor.TypeSpecificFlags & I2C_TYPE_FLAG_10BIT) return STATUS_NOT_SUPPORTED;
    if (d->SlaveAddress > I2C_MAX_ADDRESS) return STATUS_INVALID_PARAMETER;

    t->Address = d->SlaveAddress;
    t->Speed = d->ConnectionSpeed;
    return STATUS_SUCCESS;
}

static VOID Finish(SPBREQUEST Request, NTSTATUS Status, size_t Info)
{
    WdfRequestSetInformation((WDFREQUEST)Request, Info);
    SpbRequestComplete(Request, Status);
}

/* A speed request above 100 kHz means fast mode (the vendor DT sets mrvl,i2c-fast-mode on TWSI1/2). */
static VOID ApplySpeed(PGOYA_I2C I, ULONG Speed)
{
    BOOLEAN fast = Speed > 100000;
    if (fast != I->FastMode) {
        I->FastMode = fast;
        I2cResetUnit(I);
    }
}

static VOID RunRequest(WDFDEVICE Controller, SPBTARGET Target, SPBREQUEST Request, ULONG TransferCount, BOOLEAN Sequence, BOOLEAN ReadSingle)
{
    PGOYA_I2C I = GetI2c(Controller);
    PGOYA_TARGET t = GetTarget(Target);
    NTSTATUS st = STATUS_SUCCESS;
    size_t total = 0;
    ULONG idx;

    ApplySpeed(I, t->Speed);

    for (idx = 0; idx < TransferCount; idx++) {
        SPB_TRANSFER_DESCRIPTOR desc;
        PMDL mdl = NULL;
        size_t done;
        BOOLEAN last = (idx == TransferCount - 1);
        BOOLEAN read;

        SPB_TRANSFER_DESCRIPTOR_INIT(&desc);
        SpbRequestGetTransferParameters(Request, idx, &desc, &mdl);
        read = Sequence ? (desc.Direction == SpbTransferDirectionFromDevice) : ReadSingle;

        if (desc.DelayInUs && idx > 0) {
            KeStallExecutionProcessor(desc.DelayInUs);     /* sequence delay between transfers (µs, small) */
        }

        st = I2cTransfer(I, (UCHAR)t->Address, read, mdl, desc.TransferLength, last, &done);
        total += done;
        if (!NT_SUCCESS(st)) {
            I2cRecover(I);
            break;
        }
    }

    Finish(Request, st, total);
}

static VOID OnRead(WDFDEVICE Controller, SPBTARGET Target, SPBREQUEST Request, size_t Length)
{
    UNREFERENCED_PARAMETER(Length);
    RunRequest(Controller, Target, Request, 1, FALSE, TRUE);
}

static VOID OnWrite(WDFDEVICE Controller, SPBTARGET Target, SPBREQUEST Request, size_t Length)
{
    UNREFERENCED_PARAMETER(Length);
    RunRequest(Controller, Target, Request, 1, FALSE, FALSE);
}

static VOID OnSequence(WDFDEVICE Controller, SPBTARGET Target, SPBREQUEST Request, ULONG TransferCount)
{
    RunRequest(Controller, Target, Request, TransferCount, TRUE, FALSE);
}

/* ---- PnP / power ------------------------------------------------------------------- */

static NTSTATUS OnPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PGOYA_I2C I = GetI2c(Device);
    ULONG count = WdfCmResourceListGetCount(Translated), n = 0, i;
    UNREFERENCED_PARAMETER(Raw);

    for (i = 0; i < count; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = WdfCmResourceListGetDescriptor(Translated, i);
        if (r->Type != CmResourceTypeMemory) continue;
        if (n == 0) {
            if (r->u.Memory.Length < 0x40) return STATUS_DEVICE_CONFIGURATION_ERROR;
            I->Regs = (PUCHAR)MmMapIoSpace(r->u.Memory.Start, r->u.Memory.Length, MmNonCached);
            if (I->Regs == NULL) return STATUS_INSUFFICIENT_RESOURCES;
        } else if (n == 1) {
            I->ClockReg = (PULONG)MmMapIoSpace(r->u.Memory.Start, 4, MmNonCached);
        }
        n++;
    }
    return I->Regs ? STATUS_SUCCESS : STATUS_DEVICE_CONFIGURATION_ERROR;
}

static NTSTATUS OnReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    PGOYA_I2C I = GetI2c(Device);
    UNREFERENCED_PARAMETER(Translated);
    if (I->Regs) { MmUnmapIoSpace(I->Regs, 0x1000); I->Regs = NULL; }
    if (I->ClockReg) { MmUnmapIoSpace(I->ClockReg, 4); I->ClockReg = NULL; }
    return STATUS_SUCCESS;
}

static NTSTATUS OnD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Prev)
{
    PGOYA_I2C I = GetI2c(Device);
    UNREFERENCED_PARAMETER(Prev);
    I2cEnsureClock(I);
    I2cResetUnit(I);
    return STATUS_SUCCESS;
}

static NTSTATUS OnD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Target)
{
    PGOYA_I2C I = GetI2c(Device);
    UNREFERENCED_PARAMETER(Target);
    if (I->Regs) Wr(I, I2C_ICR, 0);                 /* unit off */
    return STATUS_SUCCESS;
}

static NTSTATUS OnDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT Init)
{
    NTSTATUS status;
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attrs;
    WDFDEVICE device;
    SPB_CONTROLLER_CONFIG spb;
    WDF_OBJECT_ATTRIBUTES targetAttrs;
    UNREFERENCED_PARAMETER(Driver);

    status = SpbDeviceInitConfig(Init);
    if (!NT_SUCCESS(status)) return status;

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = OnPrepareHardware;
    pnp.EvtDeviceReleaseHardware = OnReleaseHardware;
    pnp.EvtDeviceD0Entry = OnD0Entry;
    pnp.EvtDeviceD0Exit = OnD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(Init, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, GOYA_I2C);
    status = WdfDeviceCreate(&Init, &attrs, &device);
    if (!NT_SUCCESS(status)) return status;
    GetI2c(device)->Device = device;
    GetI2c(device)->FastMode = TRUE;

    SPB_CONTROLLER_CONFIG_INIT(&spb);
    spb.PowerManaged = WdfTrue;
    spb.ControllerDispatchType = WdfIoQueueDispatchSequential;
    spb.EvtSpbTargetConnect = OnTargetConnect;
    spb.EvtSpbIoRead = OnRead;
    spb.EvtSpbIoWrite = OnWrite;
    spb.EvtSpbIoSequence = OnSequence;
    status = SpbDeviceInitialize(device, &spb);
    if (!NT_SUCCESS(status)) return status;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&targetAttrs, GOYA_TARGET);
    SpbControllerSetTargetAttributes(device, &targetAttrs);
    return STATUS_SUCCESS;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG cfg;
    WDF_DRIVER_CONFIG_INIT(&cfg, OnDeviceAdd);
    cfg.DriverPoolTag = GOYA_I2C_TAG;
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
}
