/* Zinitix BT532 touchscreen (Samsung SM-T110) as a HID absolute mouse - WDM HID minidriver.

   Same model as j5touch (github.com/jimkoutso2008/j5touch): a plain WDM driver that registers with
   hidclass.sys itself (no KMDF, no mshidkmdf, no SpbCx, no goyai2c), drives the I2C controller itself
   and reports touches as an absolute mouse: the first finger moves the pointer and holds the left
   button (tap = click, drag = drag). WinPE has mouse support but no touch input.

   I2C: TWSI1 (0xD4010800, PXA "i2c-pxa" register layout), polled, 400 kHz fast mode; APBC TWSI1
   clock register 0xD4015060. The addresses are fixed for the PXA986, so the device needs no ACPI
   resources (Dsdt.asl TCH0: HID MRVL0005, no _CRS). Touch chip at 7-bit address 0x20, protocol from
   the 4.19 kernel drivers/input/touchscreen/zinitix.c (point mode 0, 5 fingers).

   A system thread polls the point-status register every 10 ms (no interrupt). Status on screen
   (top right, see common\goyatrace.h): "5 0 <status>" = result of the last chip initialisation
   (00000000 = OK, else NTSTATUS), "5 1 <chip code><firmware>" once the chip answered.
*/
#define GOYA_TRACE_ID 5
#include <ntddk.h>
#include <hidport.h>
#include "..\common\goyatrace.h"

/* ---- HID: absolute mouse, no report ID, 5-byte input report --------------------------------- */

static const UCHAR gReportDescriptor[] = {
    0x05, 0x01,             /* Usage Page (Generic Desktop) */
    0x09, 0x02,             /* Usage (Mouse) */
    0xA1, 0x01,             /* Collection (Application) */
    0x09, 0x01,             /*   Usage (Pointer) */
    0xA1, 0x00,             /*   Collection (Physical) */
    0x05, 0x09,             /*     Usage Page (Button) */
    0x19, 0x01, 0x29, 0x01, /*     Usage Minimum/Maximum (1) */
    0x15, 0x00, 0x25, 0x01, /*     Logical 0..1 */
    0x95, 0x01, 0x75, 0x01, 0x81, 0x02,     /* 1 button bit */
    0x95, 0x01, 0x75, 0x07, 0x81, 0x03,     /* 7 padding bits */
    0x05, 0x01,             /*     Usage Page (Generic Desktop) */
    0x09, 0x30, 0x09, 0x31, /*     Usage X, Y */
    0x15, 0x00, 0x26, 0xFF, 0x7F,           /* Logical 0..32767 */
    0x75, 0x10, 0x95, 0x02, 0x81, 0x02,     /* X, Y: 16 bit, absolute */
    0xC0,
    0xC0
};

static const HID_DESCRIPTOR gHidDescriptor = {
    sizeof(HID_DESCRIPTOR), 0x21, 0x0101, 0, 1, { 0x22, sizeof(gReportDescriptor) }
};

#pragma pack(push, 1)
typedef struct _MOUSE_REPORT {
    UCHAR  Buttons;         /* bit 0 = left */
    USHORT X, Y;            /* 0..32767 */
} MOUSE_REPORT;
#pragma pack(pop)

/* ---- hardware --------------------------------------------------------------------------------- */

#define TWSI1_PHYS      0xD4010800ULL
#define TWSI1_LEN       0x100
#define APBC_TWSI1_PHYS 0xD4015060ULL
#define MFPR_TWSI1_PHYS 0xD401E208ULL   /* MFPR GPIO87 (SCL), GPIO88 (SDA) */
#define MFPR_TWSI1_MASK 0x038F          /* from the vendor DT twsi1_pins (goyapinmux table) */
#define MFPR_TWSI1_VAL  0x028D          /* AF5 = TWSI1, drive/pull as in the DT */

#define I2C_IDBR   0x08
#define I2C_ICR    0x10
#define I2C_ISR    0x18
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
#define ISR_ACKNAK (1UL << 1)
#define ISR_ALD    (1UL << 5)
#define ISR_ITE    (1UL << 6)
#define ISR_IRF    (1UL << 7)
#define ISR_BED    (1UL << 10)
#define APBC_APBCLK (1UL << 0)
#define APBC_FNCLK  (1UL << 1)
#define APBC_RST    (1UL << 2)
#define I2C_POLL_LOOPS 3000         /* x 10 us = 30 ms per byte */

#define Z_ADDR     0x20

/* panel / DT: touchscreen-size 600x1024, inverted-x, swapped-x-y => screen 1024 x 600 */
#define RAW_MAX_X 600
#define RAW_MAX_Y 1024
#define NFINGERS  5
#define POLL_MS   10

#define Z_SWRESET        0x0000
#define Z_WAKEUP         0x0001
#define Z_CLEAR_INT      0x0003
#define Z_CALIBRATE      0x0006
#define Z_TOUCH_MODE     0x0010
#define Z_CHIP_REVISION  0x0011
#define Z_FW_VERSION     0x0012
#define Z_DATA_VERSION   0x0013
#define Z_FINGER_NUM     0x0015
#define Z_INIT_MODE      0x0019
#define Z_POINT_STATUS   0x0080
#define Z_BUTTON_NUM     0x00b0
#define Z_X_RES          0x00c0
#define Z_Y_RES          0x00c1
#define Z_INT_ENABLE     0x00f0
#define Z_VENDOR_ENABLE  0xc000
#define Z_PROGRAM_START  0xc001
#define Z_NVM_INIT       0xc002
#define Z_INTN_CLEAR     0xc004
#define Z_CHIP_CODE      0xcc00

#define BIT_PT_CNT_CHANGE (1u << 0)
#define BIT_DOWN          (1u << 1)
#define BIT_MOVE          (1u << 2)
#define BIT_UP            (1u << 3)
#define BIT_PT_EXIST      (1u << 11)
#define BIT_MUST_ZERO     (1u << 13)
#define SUB_BIT_EXIST     0x01
#define Z_RETRIES 8

typedef struct _DEV_EXT {
    PDEVICE_OBJECT Self;
    PDEVICE_OBJECT Next;
    PUCHAR         Twsi;            /* mapped TWSI1 */
    PULONG         Clock;           /* mapped APBC TWSI1 clock register */
    KSPIN_LOCK     Lock;
    LIST_ENTRY     PendingReads;
    PKTHREAD       Thread;
    KEVENT         StopEvent;
    BOOLEAN        Initialized;
    BOOLEAN        HaveReport;
    ULONG          InitTries;
    MOUSE_REPORT   Last;
} DEV_EXT;

static DEV_EXT *GetExt(PDEVICE_OBJECT Fdo)
{
    return (DEV_EXT *)((PHID_DEVICE_EXTENSION)Fdo->DeviceExtension)->MiniDeviceExtension;
}

static ULONG Rd(DEV_EXT *E, ULONG Off) { return READ_REGISTER_ULONG((PULONG)(E->Twsi + Off)); }
static VOID  Wr(DEV_EXT *E, ULONG Off, ULONG V) { WRITE_REGISTER_ULONG((PULONG)(E->Twsi + Off), V); }

static VOID SleepMs(ULONG Ms)
{
    LARGE_INTEGER t;
    t.QuadPart = -10000LL * Ms;
    KeDelayExecutionThread(KernelMode, FALSE, &t);
}

/* ---- polled PXA I2C master (same sequences as goyai2c / Linux i2c-pxa) --------------------- */

static VOID I2cReset(DEV_EXT *E)
{
    if (E->Clock != NULL) {
        ULONG v = READ_REGISTER_ULONG(E->Clock);
        if ((v & (APBC_APBCLK | APBC_FNCLK | APBC_RST)) != (APBC_APBCLK | APBC_FNCLK)) {
            WRITE_REGISTER_ULONG(E->Clock, (v & ~APBC_RST) | APBC_APBCLK | APBC_FNCLK);
            KeStallExecutionProcessor(10);
        }
    }
    Wr(E, I2C_ICR, ICR_UR);
    Wr(E, I2C_ISR, 0x7FF);
    Wr(E, I2C_ICR, 0);
    Wr(E, I2C_ICR, ICR_GCD | ICR_SCLE | ICR_FM);
    Wr(E, I2C_ICR, Rd(E, I2C_ICR) | ICR_IUE);
    KeStallExecutionProcessor(100);
}

static NTSTATUS I2cWait(DEV_EXT *E, ULONG Bit)
{
    ULONG n;
    for (n = 0; n < I2C_POLL_LOOPS; n++) {
        ULONG isr = Rd(E, I2C_ISR);
        if (isr & (ISR_BED | ISR_ALD)) { Wr(E, I2C_ISR, ISR_BED | ISR_ALD); return STATUS_IO_DEVICE_ERROR; }
        if (isr & Bit) { Wr(E, I2C_ISR, Bit); return STATUS_SUCCESS; }
        KeStallExecutionProcessor(10);
    }
    return STATUS_IO_TIMEOUT;
}

static NTSTATUS I2cTx(DEV_EXT *E, UCHAR Data, ULONG Flags)
{
    NTSTATUS st;
    Wr(E, I2C_IDBR, Data);
    Wr(E, I2C_ICR, (Rd(E, I2C_ICR) & ~(ICR_START | ICR_STOP | ICR_ACKNAK | ICR_TB)) | Flags | ICR_TB | ICR_IUE | ICR_SCLE);
    st = I2cWait(E, ISR_ITE);
    if (NT_SUCCESS(st) && (Rd(E, I2C_ISR) & ISR_ACKNAK)) st = STATUS_NO_SUCH_DEVICE;
    return st;
}

static NTSTATUS I2cRx(DEV_EXT *E, PUCHAR Data, BOOLEAN Last)
{
    NTSTATUS st;
    ULONG f = ICR_TB | ICR_IUE | ICR_SCLE | (Last ? (ICR_ACKNAK | ICR_STOP) : 0);
    Wr(E, I2C_ICR, (Rd(E, I2C_ICR) & ~(ICR_START | ICR_STOP | ICR_ACKNAK | ICR_TB)) | f);
    st = I2cWait(E, ISR_IRF);
    if (NT_SUCCESS(st)) *Data = (UCHAR)Rd(E, I2C_IDBR);
    return st;
}

static VOID I2cRecover(DEV_EXT *E)
{
    Wr(E, I2C_ICR, (Rd(E, I2C_ICR) & ~(ICR_START | ICR_TB | ICR_ACKNAK)) | ICR_STOP | ICR_MA);
    KeStallExecutionProcessor(50);
    I2cReset(E);
}

/* One complete write (START addr+W data... STOP) */
static NTSTATUS I2cWrite(DEV_EXT *E, const UCHAR *Buf, ULONG Len)
{
    NTSTATUS st = I2cTx(E, (UCHAR)(Z_ADDR << 1), ICR_START);
    ULONG i;
    for (i = 0; NT_SUCCESS(st) && i < Len; i++) {
        st = I2cTx(E, Buf[i], (i == Len - 1) ? ICR_STOP : 0);
    }
    if (!NT_SUCCESS(st)) I2cRecover(E);
    return st;
}

/* One complete read (START addr+R data... NAK STOP) */
static NTSTATUS I2cRead(DEV_EXT *E, PUCHAR Buf, ULONG Len)
{
    NTSTATUS st = I2cTx(E, (UCHAR)((Z_ADDR << 1) | 1), ICR_START);
    ULONG i;
    for (i = 0; NT_SUCCESS(st) && i < Len; i++) {
        st = I2cRx(E, &Buf[i], (BOOLEAN)(i == Len - 1));
    }
    if (!NT_SUCCESS(st)) I2cRecover(E);
    return st;
}

/* ---- Zinitix protocol --------------------------------------------------------------------- */

static NTSTATUS ZSend(DEV_EXT *E, const UCHAR *d, ULONG len)
{
    NTSTATUS st = STATUS_UNSUCCESSFUL;
    int i;
    for (i = 0; i < Z_RETRIES; i++) {
        st = I2cWrite(E, d, len);
        if (NT_SUCCESS(st)) return st;
        KeStallExecutionProcessor(1000);
    }
    return st;
}

static NTSTATUS ZReadData(DEV_EXT *E, USHORT reg, PUCHAR out, ULONG len)
{
    UCHAR r[2];
    NTSTATUS st;
    r[0] = (UCHAR)reg; r[1] = (UCHAR)(reg >> 8);
    st = ZSend(E, r, 2);                     /* a combined transfer does not work with this chip */
    if (!NT_SUCCESS(st)) return st;
    KeStallExecutionProcessor(50);
    st = I2cRead(E, out, len);
    KeStallExecutionProcessor(15);
    return st;
}

static NTSTATUS ZReadU16(DEV_EXT *E, USHORT reg, USHORT *v)
{
    UCHAR b[2];
    NTSTATUS st = ZReadData(E, reg, b, 2);
    if (NT_SUCCESS(st)) *v = (USHORT)(b[0] | (b[1] << 8));
    return st;
}

static NTSTATUS ZWriteU16(DEV_EXT *E, USHORT reg, USHORT val)
{
    UCHAR p[4];
    NTSTATUS st;
    p[0] = (UCHAR)reg; p[1] = (UCHAR)(reg >> 8); p[2] = (UCHAR)val; p[3] = (UCHAR)(val >> 8);
    st = ZSend(E, p, 4);
    KeStallExecutionProcessor(15);
    return st;
}

static NTSTATUS ZCmd(DEV_EXT *E, USHORT reg)
{
    UCHAR p[2];
    NTSTATUS st;
    p[0] = (UCHAR)reg; p[1] = (UCHAR)(reg >> 8);
    st = ZSend(E, p, 2);
    KeStallExecutionProcessor(15);
    return st;
}

#define CHK(x) do { st = (x); if (!NT_SUCCESS(st)) return st; } while (0)

/* zinitix_start() + power-on sequence + zinitix_init_touch() for the BT532 (mode 0) */
static NTSTATUS ZInitChip(DEV_EXT *E)
{
    NTSTATUS st;
    USHORT chip_code = 0, rev = 0, fw = 0, dv = 0;
    int i;

    I2cReset(E);
    SleepMs(15);
    ZCmd(E, Z_WAKEUP);
    SleepMs(20);

    CHK(ZWriteU16(E, Z_VENDOR_ENABLE, 1));
    CHK(ZReadU16(E, Z_CHIP_CODE, &chip_code));
    CHK(ZCmd(E, Z_INTN_CLEAR));
    CHK(ZWriteU16(E, Z_NVM_INIT, 1));
    SleepMs(2);
    CHK(ZWriteU16(E, Z_PROGRAM_START, 1));
    SleepMs(20);

    CHK(ZCmd(E, Z_SWRESET));
    SleepMs(15);
    CHK(ZReadU16(E, Z_CHIP_REVISION, &rev));
    CHK(ZReadU16(E, Z_FW_VERSION, &fw));
    CHK(ZReadU16(E, Z_DATA_VERSION, &dv));
    GoyaTraceValue(1, ((ULONG)chip_code << 16) | fw);

    CHK(ZWriteU16(E, Z_INT_ENABLE, 0));
    CHK(ZCmd(E, Z_SWRESET));
    SleepMs(15);
    CHK(ZWriteU16(E, Z_X_RES, RAW_MAX_X));
    CHK(ZWriteU16(E, Z_Y_RES, RAW_MAX_Y));
    CHK(ZWriteU16(E, Z_BUTTON_NUM, 0));
    CHK(ZWriteU16(E, Z_FINGER_NUM, NFINGERS));
    CHK(ZWriteU16(E, Z_INIT_MODE, 0));
    CHK(ZWriteU16(E, Z_TOUCH_MODE, 0));
    CHK(ZCmd(E, Z_CALIBRATE));
    CHK(ZWriteU16(E, Z_INT_ENABLE, BIT_PT_CNT_CHANGE | BIT_DOWN | BIT_MOVE | BIT_UP));
    for (i = 0; i < 10; i++) { ZCmd(E, Z_CLEAR_INT); KeStallExecutionProcessor(15); }
    return STATUS_SUCCESS;
}

/* ---- HID read completion -------------------------------------------------------------------- */

static VOID ReadCancel(PDEVICE_OBJECT Fdo, PIRP Irp)
{
    DEV_EXT *E = GetExt(Fdo);
    KIRQL irql;

    IoReleaseCancelSpinLock(Irp->CancelIrql);
    KeAcquireSpinLock(&E->Lock, &irql);
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
    KeReleaseSpinLock(&E->Lock, irql);
    Irp->IoStatus.Status = STATUS_CANCELLED;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

static PIRP TakeRead(DEV_EXT *E)
{
    KIRQL irql;
    PIRP irp = NULL;

    KeAcquireSpinLock(&E->Lock, &irql);
    while (!IsListEmpty(&E->PendingReads)) {
        PLIST_ENTRY le = RemoveHeadList(&E->PendingReads);
        irp = CONTAINING_RECORD(le, IRP, Tail.Overlay.ListEntry);
        if (IoSetCancelRoutine(irp, NULL) != NULL) break;
        InitializeListHead(&irp->Tail.Overlay.ListEntry);   /* being cancelled: ReadCancel completes it */
        irp = NULL;
    }
    KeReleaseSpinLock(&E->Lock, irql);
    return irp;
}

static VOID CompleteRead(DEV_EXT *E, const MOUSE_REPORT *R)
{
    PIRP irp = TakeRead(E);
    if (irp == NULL) return;                                /* nobody reading: the report is dropped */
    RtlCopyMemory(irp->UserBuffer, R, sizeof(*R));
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = sizeof(*R);
    IoCompleteRequest(irp, IO_MOUSE_INCREMENT);
}

static VOID CancelAllReads(DEV_EXT *E)
{
    PIRP irp;
    while ((irp = TakeRead(E)) != NULL) {
        irp->IoStatus.Status = STATUS_DEVICE_NOT_CONNECTED;
        irp->IoStatus.Information = 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
    }
}

/* ---- polling ---------------------------------------------------------------------------------- */

static VOID PollOnce(DEV_EXT *E)
{
    UCHAR raw[4 + 6 * NFINGERS];
    USHORT status;
    MOUSE_REPORT r = E->Last;
    int i;

    if (!NT_SUCCESS(ZReadData(E, Z_POINT_STATUS, raw, sizeof(raw)))) {
        E->Initialized = FALSE;                             /* chip lost: re-initialise */
        return;
    }
    status = (USHORT)(raw[0] | (raw[1] << 8));
    if (status == 0) return;                                /* nothing new */
    if (status == 0xFFFF || (status & BIT_MUST_ZERO)) { ZCmd(E, Z_CLEAR_INT); return; }

    r.Buttons = 0;
    if (status & BIT_PT_EXIST) {
        for (i = 0; i < NFINGERS; i++) {
            const UCHAR *p = raw + 4 + i * 6;               /* x(2) y(2) width sub_status */
            ULONG rx = (ULONG)(p[0] | (p[1] << 8)), ry = (ULONG)(p[2] | (p[3] << 8));
            if (!(p[5] & SUB_BIT_EXIST) || rx > RAW_MAX_X || ry > RAW_MAX_Y) continue;
            /* DT inverted-x, swapped-x-y: screen x = raw y, screen y = max - raw x */
            r.X = (USHORT)(ry * 32767 / RAW_MAX_Y);
            r.Y = (USHORT)((RAW_MAX_X - rx) * 32767 / RAW_MAX_X);
            r.Buttons = 1;
            break;
        }
    }
    ZCmd(E, Z_CLEAR_INT);

    if (!E->HaveReport || r.Buttons != E->Last.Buttons || r.X != E->Last.X || r.Y != E->Last.Y) {
        E->Last = r;
        E->HaveReport = TRUE;
        CompleteRead(E, &r);
    }
}

static VOID PollStep(DEV_EXT *E, ULONG Tick)
{
    if (!E->Initialized) {
        /* every 0.5 s for the first 20 tries, then every 5 s (a missing chip costs CPU: polled I2C) */
        if (Tick % (E->InitTries < 20 ? 50 : 500) == 0) {
            NTSTATUS st;
            E->InitTries++;
            st = ZInitChip(E);
            GoyaTraceValue(0, (ULONG)st);
            E->Initialized = (BOOLEAN)NT_SUCCESS(st);
        }
        return;
    }
    PollOnce(E);
}

static VOID PollThread(PVOID Context)
{
    DEV_EXT *E = (DEV_EXT *)Context;
    LARGE_INTEGER period;
    ULONG tick = 0;

    period.QuadPart = -10000LL * POLL_MS;
    while (KeWaitForSingleObject(&E->StopEvent, Executive, KernelMode, FALSE, &period) == STATUS_TIMEOUT) {
        __try { PollStep(E, tick); }        /* no GoyaTraceStep here: it would redraw 100 times a second */
        __except (GoyaTraceFilter(GetExceptionInformation(), 0x01)) { E->Initialized = FALSE; }
        tick++;
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* ---- PnP / power -------------------------------------------------------------------------------- */

static NTSTATUS SyncCompletion(PDEVICE_OBJECT Dev, PIRP Irp, PVOID Context)
{
    UNREFERENCED_PARAMETER(Dev);
    UNREFERENCED_PARAMETER(Irp);
    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS ForwardAndWait(DEV_EXT *E, PIRP Irp)
{
    KEVENT ev;
    KeInitializeEvent(&ev, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp, SyncCompletion, &ev, TRUE, TRUE, TRUE);
    if (IoCallDriver(E->Next, Irp) == STATUS_PENDING) {
        KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
    }
    return Irp->IoStatus.Status;
}

static VOID StopDevice(DEV_EXT *E)
{
    if (E->Thread != NULL) {
        KeSetEvent(&E->StopEvent, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(E->Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(E->Thread);
        E->Thread = NULL;
    }
    CancelAllReads(E);
    if (E->Twsi != NULL) { MmUnmapIoSpace(E->Twsi, TWSI1_LEN); E->Twsi = NULL; }
    if (E->Clock != NULL) { MmUnmapIoSpace(E->Clock, sizeof(ULONG)); E->Clock = NULL; }
    E->Initialized = FALSE;
}

/* Route GPIO87/88 to TWSI1 (normally goyapinmux does it; it is left out of WinPE cards). Only these two
   pins are touched. Slot 1 shows the previous MFPR values until the chip answers. */
static VOID SetTwsi1Pins(VOID)
{
    PHYSICAL_ADDRESS pa;
    PULONG m;
    ULONG a, b;
    pa.QuadPart = MFPR_TWSI1_PHYS;
    m = (PULONG)MmMapIoSpace(pa, 8, MmNonCached);
    if (m == NULL) return;
    a = READ_REGISTER_ULONG(&m[0]);
    b = READ_REGISTER_ULONG(&m[1]);
    GoyaTraceValue(1, (a << 16) | (b & 0xFFFF));
    if ((a & MFPR_TWSI1_MASK) != MFPR_TWSI1_VAL) { WRITE_REGISTER_ULONG(&m[0], (a & ~MFPR_TWSI1_MASK) | MFPR_TWSI1_VAL); a = READ_REGISTER_ULONG(&m[0]); }
    if ((b & MFPR_TWSI1_MASK) != MFPR_TWSI1_VAL) { WRITE_REGISTER_ULONG(&m[1], (b & ~MFPR_TWSI1_MASK) | MFPR_TWSI1_VAL); b = READ_REGISTER_ULONG(&m[1]); }
    MmUnmapIoSpace(m, 8);
}

static NTSTATUS StartDevice(DEV_EXT *E)
{
    PHYSICAL_ADDRESS pa;
    HANDLE h;
    NTSTATUS st;

    GT_GUARD(0x03, SetTwsi1Pins(), (VOID)0);

    pa.QuadPart = TWSI1_PHYS;
    E->Twsi = (PUCHAR)MmMapIoSpace(pa, TWSI1_LEN, MmNonCached);
    pa.QuadPart = APBC_TWSI1_PHYS;
    E->Clock = (PULONG)MmMapIoSpace(pa, sizeof(ULONG), MmNonCached);
    if (E->Twsi == NULL) return STATUS_INSUFFICIENT_RESOURCES;

    KeInitializeEvent(&E->StopEvent, NotificationEvent, FALSE);
    st = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, NULL, NULL, NULL, PollThread, E);
    if (!NT_SUCCESS(st)) return st;
    ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, NULL, KernelMode, (PVOID *)&E->Thread, NULL);
    ZwClose(h);
    return STATUS_SUCCESS;
}

static NTSTATUS DispatchPnp(PDEVICE_OBJECT Fdo, PIRP Irp)
{
    DEV_EXT *E = GetExt(Fdo);
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS st;

    switch (sp->MinorFunction) {
    case IRP_MN_START_DEVICE:
        st = ForwardAndWait(E, Irp);
        if (NT_SUCCESS(st)) {
            st = StartDevice(E);
            if (!NT_SUCCESS(st)) StopDevice(E);
        }
        GoyaTraceStep(NT_SUCCESS(st) ? 0x10 : 0x1F);
        Irp->IoStatus.Status = st;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return st;

    case IRP_MN_STOP_DEVICE:
    case IRP_MN_SURPRISE_REMOVAL:
    case IRP_MN_REMOVE_DEVICE:
        StopDevice(E);
        Irp->IoStatus.Status = STATUS_SUCCESS;
        break;

    default:
        break;
    }
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(E->Next, Irp);
}

static NTSTATUS DispatchPower(PDEVICE_OBJECT Fdo, PIRP Irp)
{
    PoStartNextPowerIrp(Irp);
    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(GetExt(Fdo)->Next, Irp);
}

/* ---- HID IOCTLs from hidclass (METHOD_NEITHER: output in Irp->UserBuffer) ------------------------ */

static NTSTATUS CopyOut(PIRP Irp, const VOID *Data, ULONG Length)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    if (sp->Parameters.DeviceIoControl.OutputBufferLength < Length || Irp->UserBuffer == NULL) return STATUS_BUFFER_TOO_SMALL;
    RtlCopyMemory(Irp->UserBuffer, Data, Length);
    Irp->IoStatus.Information = Length;
    return STATUS_SUCCESS;
}

static NTSTATUS DispatchInternalIoctl(PDEVICE_OBJECT Fdo, PIRP Irp)
{
    DEV_EXT *E = GetExt(Fdo);
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    HID_DEVICE_ATTRIBUTES attr;
    NTSTATUS st;
    KIRQL irql;

    Irp->IoStatus.Information = 0;
    switch (sp->Parameters.DeviceIoControl.IoControlCode) {
    case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
        st = CopyOut(Irp, &gHidDescriptor, sizeof(gHidDescriptor));
        break;
    case IOCTL_HID_GET_REPORT_DESCRIPTOR:
        st = CopyOut(Irp, gReportDescriptor, sizeof(gReportDescriptor));
        break;
    case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
        RtlZeroMemory(&attr, sizeof(attr));
        attr.Size = sizeof(attr);
        attr.VendorID = 0x1FC9;             /* arbitrary: "Goya" */
        attr.ProductID = 0x0532;
        attr.VersionNumber = 0x0003;
        st = CopyOut(Irp, &attr, sizeof(attr));
        break;
    case IOCTL_HID_READ_REPORT:
        if (sp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(MOUSE_REPORT) || Irp->UserBuffer == NULL) {
            st = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        KeAcquireSpinLock(&E->Lock, &irql);
        IoSetCancelRoutine(Irp, ReadCancel);
        if (Irp->Cancel && IoSetCancelRoutine(Irp, NULL) != NULL) {
            KeReleaseSpinLock(&E->Lock, irql);
            st = STATUS_CANCELLED;
            break;
        }
        IoMarkIrpPending(Irp);
        InsertTailList(&E->PendingReads, &Irp->Tail.Overlay.ListEntry);
        KeReleaseSpinLock(&E->Lock, irql);
        return STATUS_PENDING;
    case IOCTL_HID_ACTIVATE_DEVICE:
    case IOCTL_HID_DEACTIVATE_DEVICE:
        st = STATUS_SUCCESS;
        break;
    default:
        st = STATUS_NOT_SUPPORTED;
        break;
    }
    Irp->IoStatus.Status = st;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return st;
}

static NTSTATUS AddDevice(PDRIVER_OBJECT Driver, PDEVICE_OBJECT Fdo)
{
    DEV_EXT *E = GetExt(Fdo);
    UNREFERENCED_PARAMETER(Driver);

    RtlZeroMemory(E, sizeof(*E));
    E->Self = Fdo;
    E->Next = ((PHID_DEVICE_EXTENSION)Fdo->DeviceExtension)->NextDeviceObject;
    KeInitializeSpinLock(&E->Lock);
    InitializeListHead(&E->PendingReads);
    Fdo->Flags &= ~DO_DEVICE_INITIALIZING;
    GoyaTraceStep(0x02);
    return STATUS_SUCCESS;
}

static VOID DriverUnload(PDRIVER_OBJECT Driver)
{
    UNREFERENCED_PARAMETER(Driver);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING RegistryPath)
{
    HID_MINIDRIVER_REGISTRATION reg;

    Driver->MajorFunction[IRP_MJ_PNP] = DispatchPnp;
    Driver->MajorFunction[IRP_MJ_POWER] = DispatchPower;
    Driver->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = DispatchInternalIoctl;
    Driver->DriverExtension->AddDevice = AddDevice;
    Driver->DriverUnload = DriverUnload;

    RtlZeroMemory(&reg, sizeof(reg));
    reg.Revision = HID_REVISION;
    reg.DriverObject = Driver;
    reg.RegistryPath = RegistryPath;
    reg.DeviceExtensionSize = sizeof(DEV_EXT);
    reg.DevicesArePolled = FALSE;
    GoyaTraceStep(0x00);
    return HidRegisterMinidriver(&reg);
}
