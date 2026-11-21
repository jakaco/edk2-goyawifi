/* Zinitix BT532 touchscreen (Samsung SM-T110) - KMDF HID source driver for Windows 8.0 RT / 10.

   Ported from the 4.19 kernel drivers/input/touchscreen/zinitix.c (point mode 0, 5 fingers).
   The chip speaks a proprietary 16-bit-register I2C protocol, not HID-over-I2C, so this driver talks to
   it through the SpbCx I2C controller (goyai2c) and exposes a standard HID multitouch digitizer to
   Windows as a lower filter under mshidkmdf.sys (see common\hidmini.h).

   No interrupt is used: a 16 ms passive-level timer polls the point-status register (like the vendor ISR
   does after each INT) and clears the status after each event. The chip is (re)initialised from the
   timer whenever it is not initialised, so a late-starting I2C controller or LDO is handled.

   ACPI (see Dsdt.asl TCH0): HID MRVL0005 with I2cSerialBus(0x20, 400 kHz, "\_SB.I2C1").
*/
#define GOYA_TRACE_ID 5
#include "..\common\hidmini.h"
#include "..\common\spbtarget.h"

#define GT_TAG 'tyoG'

/* panel / DT: touchscreen-size 600x1024, inverted-x, swapped-x-y  =>  digitizer 1024 x 600 */
#define RAW_MAX_X 600
#define RAW_MAX_Y 1024
#define OUT_MAX_X 1024
#define OUT_MAX_Y 600
#define NFINGERS  5

/* registers */
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

/* HID report descriptor: Windows 8 touch screen, 5 fingers, parallel mode.
   Input report 1: 5 x {tip(1)+pad(7), contact id(8), x(16), y(16)} + contact count(8) = 31 bytes + ID.
   Feature report 2: contact count maximum. Physical size 7" panel: 152.4 x 88.9 mm (0.01 cm units). */
#define FINGER_COLLECTION \
    0x09, 0x22,             /* Usage (Finger) */ \
    0xA1, 0x02,             /* Collection (Logical) */ \
    0x09, 0x42,             /*  Usage (Tip Switch) */ \
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02, \
    0x75, 0x07, 0x95, 0x01, 0x81, 0x03,                         /* pad */ \
    0x09, 0x51,             /*  Usage (Contact Identifier) */ \
    0x75, 0x08, 0x95, 0x01, 0x15, 0x00, 0x25, 0x0F, 0x81, 0x02, \
    0x05, 0x01,             /*  Usage Page (Generic Desktop) */ \
    0x09, 0x30,             /*  Usage (X) */ \
    0x75, 0x10, 0x95, 0x01, 0x15, 0x00, 0x26, 0x00, 0x04, \
    0x35, 0x00, 0x46, 0xF4, 0x05, 0x65, 0x11, 0x55, 0x0E, 0x81, 0x02, \
    0x09, 0x31,             /*  Usage (Y) */ \
    0x26, 0x58, 0x02, 0x46, 0x79, 0x03, 0x81, 0x02, \
    0x05, 0x0D,             /*  Usage Page (Digitizer) */ \
    0xC0

static const UCHAR gReportDesc[] = {
    0x05, 0x0D,             /* Usage Page (Digitizer) */
    0x09, 0x04,             /* Usage (Touch Screen) */
    0xA1, 0x01,             /* Collection (Application) */
    0x85, 0x01,             /*  Report ID (1) */
    FINGER_COLLECTION, FINGER_COLLECTION, FINGER_COLLECTION, FINGER_COLLECTION, FINGER_COLLECTION,
    0x09, 0x54,             /*  Usage (Contact Count) */
    0x75, 0x08, 0x95, 0x01, 0x15, 0x00, 0x25, 0x05, 0x81, 0x02,
    0x85, 0x02,             /*  Report ID (2) */
    0x09, 0x55,             /*  Usage (Contact Count Maximum) */
    0x25, 0x05, 0xB1, 0x02,
    0xC0
    /* An absolute mouse collection (report 3) here coincided with KMODE_EXCEPTION_NOT_HANDLED
       on 8520 WinPE; build with GOYA_TOUCH_MOUSE=1 and add it back to experiment. */
};

#pragma pack(push, 1)
typedef struct _MOUSE_REPORT {
    UCHAR  Id;              /* 3 */
    UCHAR  Buttons;
    USHORT X, Y;            /* 0..32767 */
} MOUSE_REPORT;
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct _TOUCH_REPORT {
    UCHAR Id;
    struct { UCHAR Tip; UCHAR ContactId; USHORT X; USHORT Y; } F[NFINGERS];
    UCHAR Count;
} TOUCH_REPORT;
#pragma pack(pop)

typedef struct _GOYA_TOUCH {
    WDFDEVICE Device;
    WDFTIMER  Timer;
    SPB_TGT   Spb;
    BOOLEAN   Running;
    BOOLEAN   Initialized;
    ULONG     RetryTicks;
    LONG      InTimer;
    UCHAR     PrevActive;           /* bitmask of fingers that were down in the last report */
    USHORT    MouseX, MouseY;       /* last pointer position (report 3) */
} GOYA_TOUCH, *PGOYA_TOUCH;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(GOYA_TOUCH, GetTouch)

/* ---- chip access -------------------------------------------------------------------- */

static NTSTATUS ZSend(PGOYA_TOUCH T, const UCHAR *d, ULONG len)
{
    NTSTATUS st = STATUS_UNSUCCESSFUL;
    int i;
    for (i = 0; i < Z_RETRIES; i++) {
        st = SpbTgtWrite(&T->Spb, (PVOID)d, len);
        if (NT_SUCCESS(st)) return st;
        KeStallExecutionProcessor(1000);
    }
    return st;
}

static NTSTATUS ZReadData(PGOYA_TOUCH T, USHORT reg, PVOID out, ULONG len)
{
    UCHAR r[2];
    NTSTATUS st;
    r[0] = (UCHAR)reg; r[1] = (UCHAR)(reg >> 8);
    st = ZSend(T, r, 2);                 /* a combined transfer does not work with this chip */
    if (!NT_SUCCESS(st)) return st;
    KeStallExecutionProcessor(50);
    st = SpbTgtRead(&T->Spb, out, len);
    KeStallExecutionProcessor(15);
    return st;
}

static NTSTATUS ZReadU16(PGOYA_TOUCH T, USHORT reg, USHORT *v)
{
    UCHAR b[2];
    NTSTATUS st = ZReadData(T, reg, b, 2);
    if (NT_SUCCESS(st)) *v = (USHORT)(b[0] | (b[1] << 8));
    return st;
}

static NTSTATUS ZWriteU16(PGOYA_TOUCH T, USHORT reg, USHORT val)
{
    UCHAR p[4];
    NTSTATUS st;
    p[0] = (UCHAR)reg; p[1] = (UCHAR)(reg >> 8); p[2] = (UCHAR)val; p[3] = (UCHAR)(val >> 8);
    st = ZSend(T, p, 4);
    KeStallExecutionProcessor(15);
    return st;
}

static NTSTATUS ZCmd(PGOYA_TOUCH T, USHORT reg)
{
    UCHAR p[2];
    NTSTATUS st;
    p[0] = (UCHAR)reg; p[1] = (UCHAR)(reg >> 8);
    st = ZSend(T, p, 2);
    KeStallExecutionProcessor(15);
    return st;
}

#define CHK(x) do { st = (x); if (!NT_SUCCESS(st)) { DbgPrint("goyatouch: init step failed line %d: 0x%08x\n", __LINE__, st); return st; } } while (0)

/* zinitix_start() + zinitix_send_power_on_sequence() + zinitix_init_touch() for the BT532 (mode 0) */
static NTSTATUS ZInitChip(PGOYA_TOUCH T)
{
    NTSTATUS st;
    USHORT chip_code = 0, rev = 0, fw = 0, dv = 0, flags;
    int i;

    SleepMs(15);                                       /* CHIP_ON_DELAY */
    ZCmd(T, Z_WAKEUP);                                 /* keep_powered_idle chips: wake first */
    SleepMs(20);

    CHK(ZWriteU16(T, Z_VENDOR_ENABLE, 1));
    CHK(ZReadU16(T, Z_CHIP_CODE, &chip_code));
    CHK(ZCmd(T, Z_INTN_CLEAR));
    CHK(ZWriteU16(T, Z_NVM_INIT, 1));
    SleepMs(2);
    CHK(ZWriteU16(T, Z_PROGRAM_START, 1));
    SleepMs(20);                                       /* FIRMWARE_ON_DELAY */

    CHK(ZCmd(T, Z_SWRESET));
    SleepMs(15);                                       /* bt532 reset_delay_ms */
    CHK(ZReadU16(T, Z_CHIP_REVISION, &rev));
    CHK(ZReadU16(T, Z_FW_VERSION, &fw));
    CHK(ZReadU16(T, Z_DATA_VERSION, &dv));
    DbgPrint("goyatouch: chip code %04x revision %04x firmware %04x data %04x\n", chip_code, rev, fw, dv);
    GoyaTraceValue(1, ((ULONG)chip_code << 16) | fw);   /* 5 1 <chip code><firmware> */

    CHK(ZWriteU16(T, Z_INT_ENABLE, 0));
    CHK(ZCmd(T, Z_SWRESET));
    SleepMs(15);
    CHK(ZWriteU16(T, Z_X_RES, RAW_MAX_X));
    CHK(ZWriteU16(T, Z_Y_RES, RAW_MAX_Y));
    CHK(ZWriteU16(T, Z_BUTTON_NUM, 0));
    CHK(ZWriteU16(T, Z_FINGER_NUM, NFINGERS));
    CHK(ZWriteU16(T, Z_INIT_MODE, 0));                 /* zinitix,mode = <0> */
    CHK(ZWriteU16(T, Z_TOUCH_MODE, 0));
    CHK(ZCmd(T, Z_CALIBRATE));
    flags = BIT_PT_CNT_CHANGE | BIT_DOWN | BIT_MOVE | BIT_UP;
    CHK(ZWriteU16(T, Z_INT_ENABLE, flags));
    for (i = 0; i < 10; i++) { ZCmd(T, Z_CLEAR_INT); KeStallExecutionProcessor(15); }
    return STATUS_SUCCESS;
}

/* ---- one poll ------------------------------------------------------------------------ */

static USHORT Clamp(LONG v, LONG max) { return (USHORT)(v < 0 ? 0 : (v > max ? max : v)); }

static VOID PollOnce(PGOYA_TOUCH T)
{
    UCHAR raw[4 + 6 * NFINGERS];
    TOUCH_REPORT rep;
    USHORT status;
    UCHAR active = 0;
    int i, count = 0;
    NTSTATUS st;

    st = ZReadData(T, Z_POINT_STATUS, raw, sizeof(raw));
    if (!NT_SUCCESS(st)) { T->Initialized = FALSE; T->RetryTicks = 0; return; }   /* chip lost: re-init */

    status = (USHORT)(raw[0] | (raw[1] << 8));
    if (status == 0) return;                                   /* nothing new */
    if (status == 0xFFFF || (status & BIT_MUST_ZERO)) { ZCmd(T, Z_CLEAR_INT); return; }

    RtlZeroMemory(&rep, sizeof(rep));
    rep.Id = 1;

    if (status & BIT_PT_EXIST) {
        for (i = 0; i < NFINGERS; i++) {
            const UCHAR *p = raw + 4 + i * 6;                  /* x(2) y(2) width sub_status */
            USHORT rx = (USHORT)(p[0] | (p[1] << 8)), ry = (USHORT)(p[2] | (p[3] << 8));
            if (!(p[5] & SUB_BIT_EXIST)) continue;
            if (rx > RAW_MAX_X || ry > RAW_MAX_Y) continue;
            /* DT: inverted-x then swapped-x-y */
            rep.F[count].Tip = 1;
            rep.F[count].ContactId = (UCHAR)i;
            rep.F[count].X = Clamp(ry, OUT_MAX_X);
            rep.F[count].Y = Clamp(RAW_MAX_X - (LONG)rx, OUT_MAX_Y);
            active |= (UCHAR)(1u << i);
            count++;
        }
    }
    /* fingers that were down before but are not now: send one tip-up each (keep last known position 0) */
    for (i = 0; i < NFINGERS && count < NFINGERS; i++) {
        if ((T->PrevActive & (1u << i)) && !(active & (1u << i))) {
            rep.F[count].Tip = 0;
            rep.F[count].ContactId = (UCHAR)i;
            count++;
        }
    }
    rep.Count = (UCHAR)count;
    if (count || T->PrevActive) {
        HidMiniReport(T->Device, (const UCHAR *)&rep, sizeof(rep));
#if GOYA_TOUCH_MOUSE
        MOUSE_REPORT m;
        /* pointer follows the first finger that is down; button up when none is */
        m.Id = 3;
        m.Buttons = 0;
        m.X = T->MouseX;
        m.Y = T->MouseY;
        for (i = 0; i < count; i++) {
            if (rep.F[i].Tip) {
                m.Buttons = 1;
                m.X = T->MouseX = (USHORT)((ULONG)rep.F[i].X * 32767 / OUT_MAX_X);
                m.Y = T->MouseY = (USHORT)((ULONG)rep.F[i].Y * 32767 / OUT_MAX_Y);
                break;
            }
        }
        HidMiniReport(T->Device, (const UCHAR *)&m, sizeof(m));
#endif
    }
    T->PrevActive = active;
    ZCmd(T, Z_CLEAR_INT);
}

static VOID OnTimer_(WDFTIMER Timer)
{
    WDFDEVICE dev = (WDFDEVICE)WdfTimerGetParentObject(Timer);
    PGOYA_TOUCH T = GetTouch(dev);

    if (!T->Running) return;
    if (InterlockedExchange(&T->InTimer, 1)) return;           /* a slow poll must not overlap */

    if (!SpbTgtOpen(dev, &T->Spb)) {
        GoyaTraceValue(0, 0xDEAD0001);                         /* 5 0 DEAD0001: I2C target cannot be opened */
    } else {
        if (!T->Initialized) {
            if (T->RetryTicks++ % 32 == 0) {                   /* about every 0.5 s */
                NTSTATUS ist = ZInitChip(T);
            GoyaTraceValue(0, (ULONG)ist);          /* 5 0 <status>: 00000000 = touch chip initialised */
            if (NT_SUCCESS(ist)) { T->Initialized = TRUE; T->PrevActive = 0; }
            }
        } else {
            PollOnce(T);
        }
    }
    InterlockedExchange(&T->InTimer, 0);
    if (T->Running) WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(16));
}

static VOID OnTimer(WDFTIMER Timer)
{
    GT_GUARD(0x01, OnTimer_(Timer), ;);
}

/* ---- HID feature reports ---------------------------------------------------------- */

static NTSTATUS OnGetFeature_(PVOID Ctx, UCHAR ReportId, PUCHAR Buf, ULONG Len, PULONG Written)
{
    UNREFERENCED_PARAMETER(Ctx);
    if (ReportId == 2 && Len >= 2) {
        Buf[0] = 2;
        Buf[1] = NFINGERS;
        *Written = 2;
        return STATUS_SUCCESS;
    }
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS OnGetFeature(PVOID Ctx, UCHAR ReportId, PUCHAR Buf, ULONG Len, PULONG Written)
{
    NTSTATUS r_ = STATUS_UNSUCCESSFUL;
    GT_GUARD(0x07, r_ = OnGetFeature_(Ctx, ReportId, Buf, Len, Written), r_ = STATUS_UNSUCCESSFUL);
    return r_;
}

/* ---- PnP / power -------------------------------------------------------------------- */

static NTSTATUS OnPrepareHardware_(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PGOYA_TOUCH T = GetTouch(Device);
    UNREFERENCED_PARAMETER(Raw);
    if (!SpbTgtFindResource(Translated, &T->Spb)) {
        DbgPrint("goyatouch: no I2C connection resource\n");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS OnPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    NTSTATUS r_ = STATUS_UNSUCCESSFUL;
    GT_GUARD(0x02, r_ = OnPrepareHardware_(Device, Raw, Translated), r_ = STATUS_UNSUCCESSFUL);
    return r_;
}

static NTSTATUS OnReleaseHardware_(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    PGOYA_TOUCH T = GetTouch(Device);
    UNREFERENCED_PARAMETER(Translated);
    SpbTgtClose(&T->Spb);
    return STATUS_SUCCESS;
}

static NTSTATUS OnReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    NTSTATUS r_ = STATUS_SUCCESS;
    GT_GUARD(0x03, r_ = OnReleaseHardware_(Device, Translated), r_ = STATUS_SUCCESS);
    return r_;
}

static NTSTATUS OnD0Entry_(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Prev)
{
    PGOYA_TOUCH T = GetTouch(Device);
    UNREFERENCED_PARAMETER(Prev);
    T->Initialized = FALSE;
    T->RetryTicks = 0;
    T->PrevActive = 0;
    T->Running = TRUE;
    WdfTimerStart(T->Timer, WDF_REL_TIMEOUT_IN_MS(16));
    return STATUS_SUCCESS;
}

static NTSTATUS OnD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Prev)
{
    NTSTATUS r_ = STATUS_UNSUCCESSFUL;
    GT_GUARD(0x04, r_ = OnD0Entry_(Device, Prev), r_ = STATUS_UNSUCCESSFUL);
    return r_;
}

static NTSTATUS OnD0Exit_(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Target)
{
    PGOYA_TOUCH T = GetTouch(Device);
    UNREFERENCED_PARAMETER(Target);
    T->Running = FALSE;
    WdfTimerStop(T->Timer, TRUE);
    if (T->Initialized && T->Spb.Opened) {
        ZWriteU16(T, Z_INT_ENABLE, 0);                  /* quiet the chip while the system sleeps */
    }
    T->Initialized = FALSE;
    return STATUS_SUCCESS;
}

static NTSTATUS OnD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Target)
{
    NTSTATUS r_ = STATUS_SUCCESS;
    GT_GUARD(0x05, r_ = OnD0Exit_(Device, Target), r_ = STATUS_SUCCESS);
    return r_;
}

static NTSTATUS OnDeviceAdd_(WDFDRIVER Driver, PWDFDEVICE_INIT Init)
{
    NTSTATUS st;
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES a;
    WDF_TIMER_CONFIG tc;
    WDFDEVICE dev;
    HIDMINI_CONFIG hc;
    PGOYA_TOUCH T;
    UNREFERENCED_PARAMETER(Driver);

    WdfFdoInitSetFilter(Init);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = OnPrepareHardware;
    pnp.EvtDeviceReleaseHardware = OnReleaseHardware;
    pnp.EvtDeviceD0Entry = OnD0Entry;
    pnp.EvtDeviceD0Exit = OnD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(Init, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a, GOYA_TOUCH);
    a.ExecutionLevel = WdfExecutionLevelPassive;       /* synchronous I/O from the timer */
    st = WdfDeviceCreate(&Init, &a, &dev);
    if (!NT_SUCCESS(st)) return st;
    T = GetTouch(dev);
    T->Device = dev;

    RtlZeroMemory(&hc, sizeof(hc));
    hc.ReportDescriptor = gReportDesc;
    hc.ReportDescriptorLength = sizeof(gReportDesc);
    hc.VendorId = 0x1FC9;                              /* arbitrary: "Goya" */
    hc.ProductId = 0x0532;
    hc.VersionNumber = 0x0001;
    hc.GetFeature = OnGetFeature;
    hc.Ctx = T;
    st = HidMiniCreate(dev, &hc);
    if (!NT_SUCCESS(st)) return st;

    /* KMDF: a passive-level timer cannot be periodic (WdfTimerCreate rejects a non-zero Period), so
       this is a one-shot timer that OnTimer re-arms while the device is in D0. */
    WDF_TIMER_CONFIG_INIT(&tc, OnTimer);
    tc.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&a);
    a.ParentObject = dev;
    a.ExecutionLevel = WdfExecutionLevelPassive;
    return WdfTimerCreate(&tc, &a, &T->Timer);
}

static NTSTATUS OnDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT Init)
{
    NTSTATUS r_ = STATUS_UNSUCCESSFUL;
    GT_GUARD(0x06, r_ = OnDeviceAdd_(Driver, Init), r_ = STATUS_UNSUCCESSFUL);
    return r_;
}

static NTSTATUS DriverEntry_(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG cfg;
    WDF_DRIVER_CONFIG_INIT(&cfg, OnDeviceAdd);
    cfg.DriverPoolTag = GT_TAG;
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    NTSTATUS r_ = STATUS_UNSUCCESSFUL;
    GT_GUARD(0x00, r_ = DriverEntry_(DriverObject, RegistryPath), r_ = STATUS_UNSUCCESSFUL);
    return r_;
}
