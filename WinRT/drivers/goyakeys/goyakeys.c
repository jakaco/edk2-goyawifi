/* Hardware keys of the Galaxy Tab 3 Lite (SM-T110) - KMDF HID source driver for Windows 8.0 RT / 10.

   Buttons (from pxa986-samsung-goyawifi.dts gpio-keys, all active low):
     Volume Up   GPIO 1    -> Consumer "Volume Increment"
     Volume Down GPIO 2    -> Consumer "Volume Decrement"
     Home        GPIO 13   -> Keyboard Left GUI (the Windows key)
     Power       88PM822 STATUS_1 bit 0 (ONKEY_STS1, read over I2C) -> System Control "System Power Down"

   GPIO levels are read straight from the GPLR register of the PXA GPIO block (0xD4019000, bank 0, read
   only, no interrupt; S-Boot/firmware already muxed the pins and enabled the pull-ups). The block is
   not claimed in ACPI because goyagpio owns it; reading GPLR is harmless. The power key is read through
   an optional I2cSerialBus resource to the PMIC (address 0x30 on TWSI2).

   A 16 ms passive-level timer samples the keys; a change must be seen in two consecutive samples
   (>= 16 ms, like the vendor 30 ms debounce) before it is reported.

   ACPI (see Dsdt.asl KEY0): HID MRVL0006.
*/
#include "..\common\hidmini.h"
#include "..\common\spbtarget.h"

#define GK_TAG 'ykoG'

#define GPIO_BASE_PHYS  0xD4019000ULL
#define GPIO_MAP_LEN    0x100
#define GPLR0           0x00

#define PIN_VOLUP   1
#define PIN_VOLDN   2
#define PIN_HOME    13

#define PM_ADDR_STATUS1 0x01
#define PM_ONKEY_STS    0x01

/* key bit positions in our state word */
#define K_VOLUP  0x1
#define K_VOLDN  0x2
#define K_HOME   0x4
#define K_POWER  0x8

static const UCHAR gReportDesc[] = {
    /* Report 1: consumer volume */
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01,
      0x85, 0x01,
      0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02,
      0x09, 0xE9, 0x09, 0xEA, 0x81, 0x02,
      0x75, 0x06, 0x95, 0x01, 0x81, 0x03,
    0xC0,
    /* Report 2: keyboard (modifiers + reserved + 6 keys) */
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,
      0x85, 0x02,
      0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
      0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
      0x75, 0x08, 0x95, 0x01, 0x81, 0x03,
      0x75, 0x08, 0x95, 0x06, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xC0,
    /* Report 3: system control (power) */
    0x05, 0x01, 0x09, 0x80, 0xA1, 0x01,
      0x85, 0x03,
      0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01,
      0x09, 0x81, 0x81, 0x02,
      0x75, 0x07, 0x95, 0x01, 0x81, 0x03,
    0xC0
};

typedef struct _GOYA_KEYS {
    WDFDEVICE Device;
    WDFTIMER  Timer;
    PUCHAR    Gpio;               /* mapped GPLR page */
    SPB_TGT   Spb;                /* optional PMIC bus */
    BOOLEAN   Running;
    LONG      InTimer;
    ULONG     Reported;           /* last reported state */
    ULONG     Candidate;          /* last sample */
} GOYA_KEYS, *PGOYA_KEYS;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(GOYA_KEYS, GetKeys)

static ULONG SampleKeys(PGOYA_KEYS K)
{
    ULONG s = 0, lvl;
    lvl = READ_REGISTER_ULONG((PULONG)(K->Gpio + GPLR0));
    if (!(lvl & (1UL << PIN_VOLUP))) s |= K_VOLUP;           /* active low */
    if (!(lvl & (1UL << PIN_VOLDN))) s |= K_VOLDN;
    if (!(lvl & (1UL << PIN_HOME)))  s |= K_HOME;

    if (K->Spb.HaveId && SpbTgtOpen(K->Device, &K->Spb)) {
        UCHAR reg = PM_ADDR_STATUS1, val = 0;
        if (NT_SUCCESS(SpbTgtWrite(&K->Spb, &reg, 1)) && NT_SUCCESS(SpbTgtRead(&K->Spb, &val, 1))) {
            if (val & PM_ONKEY_STS) s |= K_POWER;
        } else {
            s |= (K->Reported & K_POWER);                    /* bus error: keep previous power state */
        }
    }
    return s;
}

static VOID SendChanges(PGOYA_KEYS K, ULONG now)
{
    ULONG diff = now ^ K->Reported;
    UCHAR r[9];

    if (diff & (K_VOLUP | K_VOLDN)) {
        r[0] = 1;
        r[1] = (UCHAR)(((now & K_VOLUP) ? 1 : 0) | ((now & K_VOLDN) ? 2 : 0));
        HidMiniReport(K->Device, r, 2);
    }
    if (diff & K_HOME) {
        RtlZeroMemory(r, sizeof(r));
        r[0] = 2;
        r[1] = (now & K_HOME) ? 0x08 : 0x00;                 /* Left GUI modifier */
        HidMiniReport(K->Device, r, 9);
    }
    if (diff & K_POWER) {
        r[0] = 3;
        r[1] = (now & K_POWER) ? 1 : 0;
        HidMiniReport(K->Device, r, 2);
    }
    K->Reported = now;
}

static VOID OnTimer(WDFTIMER Timer)
{
    WDFDEVICE dev = (WDFDEVICE)WdfTimerGetParentObject(Timer);
    PGOYA_KEYS K = GetKeys(dev);
    ULONG now;

    if (!K->Running || K->Gpio == NULL) return;
    if (InterlockedExchange(&K->InTimer, 1)) return;

    now = SampleKeys(K);
    if (now == K->Candidate && now != K->Reported) SendChanges(K, now);   /* stable for two samples */
    K->Candidate = now;

    InterlockedExchange(&K->InTimer, 0);
    if (K->Running) WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(16));
}

static NTSTATUS OnPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PGOYA_KEYS K = GetKeys(Device);
    PHYSICAL_ADDRESS pa;
    UNREFERENCED_PARAMETER(Raw);

    SpbTgtFindResource(Translated, &K->Spb);                 /* optional: power key */

    pa.QuadPart = GPIO_BASE_PHYS;
    K->Gpio = (PUCHAR)MmMapIoSpace(pa, GPIO_MAP_LEN, MmNonCached);
    return K->Gpio ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}

static NTSTATUS OnReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    PGOYA_KEYS K = GetKeys(Device);
    UNREFERENCED_PARAMETER(Translated);
    SpbTgtClose(&K->Spb);
    if (K->Gpio) { MmUnmapIoSpace(K->Gpio, GPIO_MAP_LEN); K->Gpio = NULL; }
    return STATUS_SUCCESS;
}

static NTSTATUS OnD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Prev)
{
    PGOYA_KEYS K = GetKeys(Device);
    UNREFERENCED_PARAMETER(Prev);
    K->Reported = K->Candidate = 0;
    K->Running = TRUE;
    WdfTimerStart(K->Timer, WDF_REL_TIMEOUT_IN_MS(16));
    return STATUS_SUCCESS;
}

static NTSTATUS OnD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Target)
{
    PGOYA_KEYS K = GetKeys(Device);
    UNREFERENCED_PARAMETER(Target);
    K->Running = FALSE;
    WdfTimerStop(K->Timer, TRUE);
    return STATUS_SUCCESS;
}

static NTSTATUS OnDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT Init)
{
    NTSTATUS st;
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES a;
    WDF_TIMER_CONFIG tc;
    WDFDEVICE dev;
    HIDMINI_CONFIG hc;
    PGOYA_KEYS K;
    UNREFERENCED_PARAMETER(Driver);

    WdfFdoInitSetFilter(Init);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = OnPrepareHardware;
    pnp.EvtDeviceReleaseHardware = OnReleaseHardware;
    pnp.EvtDeviceD0Entry = OnD0Entry;
    pnp.EvtDeviceD0Exit = OnD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(Init, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a, GOYA_KEYS);
    a.ExecutionLevel = WdfExecutionLevelPassive;
    st = WdfDeviceCreate(&Init, &a, &dev);
    if (!NT_SUCCESS(st)) return st;
    K = GetKeys(dev);
    K->Device = dev;

    RtlZeroMemory(&hc, sizeof(hc));
    hc.ReportDescriptor = gReportDesc;
    hc.ReportDescriptorLength = sizeof(gReportDesc);
    hc.VendorId = 0x1FC9;
    hc.ProductId = 0x0001;
    hc.VersionNumber = 0x0001;
    st = HidMiniCreate(dev, &hc);
    if (!NT_SUCCESS(st)) return st;

    /* KMDF: a passive-level timer cannot be periodic (WdfTimerCreate rejects a non-zero Period), so
       this is a one-shot timer that OnTimer re-arms while the device is in D0. */
    WDF_TIMER_CONFIG_INIT(&tc, OnTimer);
    tc.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&a);
    a.ParentObject = dev;
    a.ExecutionLevel = WdfExecutionLevelPassive;
    return WdfTimerCreate(&tc, &a, &K->Timer);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG cfg;
    WDF_DRIVER_CONFIG_INIT(&cfg, OnDeviceAdd);
    cfg.DriverPoolTag = GK_TAG;
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
}
