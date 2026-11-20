/* Pin multiplexer driver for the Marvell PXA986/PXA988 (Galaxy Tab 3 Lite, SM-T110) - KMDF.

   Windows 8.0 has no pin-control class extension (PinFunction resources arrived with Windows 10), so this
   is a small function driver that owns the MFPR register block (0xD401E000, 0x330 bytes) and:
     - applies the board's pin configuration (alternate function, pull, drive strength, sleep mode) at D0Entry
       from pinmux_table.h, which tools/gen-pinmux-table.py generates from the vendor DT pin groups
       (pxa986-samsung-goyawifi-pins.dtsi); read-modify-write, only the masked bits are touched;
     - offers IOCTL_GOYAPINMUX_GET / _SET (read / read-modify-write one MFPR) on a device interface so other
       components can mux pins at run time.

   Registry (device hardware key, optional):
     GroupMask (REG_DWORD)  bit n enables table group n (order as in pinmux_table.h). Default
                            PINMUX_DEFAULT_MASK = every group except the ones Windows may be using at the
                            moment the driver starts: sdh1 (microSD = boot disk), sdh3 (eMMC), uart1
                            (debug port) and the TWSI1 "gpio" idle state.
     Disable   (REG_DWORD)  non-zero: do not apply the static table at all.

   ACPI (see Dsdt.asl PMX0): HID MRVL0008, Memory32Fixed 0xD401E000 / 0x1000.
*/
#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>
#include "pinmux_table.h"

#define PM_TAG 'xmPG'
#define MFPR_PHYS   0xD401E000ULL
#define MFPR_LEN    0x330

/* {A4C5D8E1-5B0E-4F29-9C7B-3F1E6A2D8B44} */
DEFINE_GUID(GUID_DEVINTERFACE_GOYAPINMUX, 0xa4c5d8e1, 0x5b0e, 0x4f29, 0x9c, 0x7b, 0x3f, 0x1e, 0x6a, 0x2d, 0x8b, 0x44);

#define IOCTL_GOYAPINMUX_GET CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_GOYAPINMUX_SET CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_WRITE_ACCESS)

typedef struct _PINMUX_REQ { ULONG Offset; ULONG Clear; ULONG Set; } PINMUX_REQ;

typedef struct _GOYA_PINMUX {
    PUCHAR Base;
    ULONG  Applied;                     /* registers changed by the last D0Entry */
} GOYA_PINMUX, *PGOYA_PINMUX;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(GOYA_PINMUX, GetPm)

static __forceinline BOOLEAN OffsetOk(ULONG o) { return o + sizeof(ULONG) <= MFPR_LEN && (o & 3) == 0; }

static ULONG ReadDword(WDFKEY Key, PCWSTR Name, ULONG Default)
{
    UNICODE_STRING n;
    ULONG v = Default;
    RtlInitUnicodeString(&n, Name);
    if (!NT_SUCCESS(WdfRegistryQueryULong(Key, &n, &v))) v = Default;
    return v;
}

/* Read-modify-write; registers that already hold the wanted value are not written (no glitches). */
static BOOLEAN Rmw(PGOYA_PINMUX P, ULONG Off, ULONG Clear, ULONG Set)
{
    PULONG r = (PULONG)(P->Base + Off);
    ULONG v = READ_REGISTER_ULONG(r);
    ULONG n = (v & ~Clear) | (Set & Clear);
    if (n == v) return FALSE;
    WRITE_REGISTER_ULONG(r, n);
    (VOID)READ_REGISTER_ULONG(r);       /* post the write before the next pad changes */
    return TRUE;
}

static NTSTATUS OnPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PGOYA_PINMUX P = GetPm(Device);
    ULONG i, n = WdfCmResourceListGetCount(Translated);
    UNREFERENCED_PARAMETER(Raw);
    for (i = 0; i < n; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = WdfCmResourceListGetDescriptor(Translated, i);
        if (r->Type == CmResourceTypeMemory && r->u.Memory.Length >= MFPR_LEN) {
            P->Base = (PUCHAR)MmMapIoSpace(r->u.Memory.Start, MFPR_LEN, MmNonCached);
            break;
        }
    }
    return P->Base ? STATUS_SUCCESS : STATUS_DEVICE_CONFIGURATION_ERROR;
}

static NTSTATUS OnReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    PGOYA_PINMUX P = GetPm(Device);
    UNREFERENCED_PARAMETER(Translated);
    if (P->Base) { MmUnmapIoSpace(P->Base, MFPR_LEN); P->Base = NULL; }
    return STATUS_SUCCESS;
}

static NTSTATUS OnD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Prev)
{
    PGOYA_PINMUX P = GetPm(Device);
    ULONG mask = PINMUX_DEFAULT_MASK, disable = 0, i;
    WDFKEY key;
    UNREFERENCED_PARAMETER(Prev);

    if (NT_SUCCESS(WdfDeviceOpenRegistryKey(Device, PLUGPLAY_REGKEY_DEVICE, KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        mask = ReadDword(key, L"GroupMask", mask);
        disable = ReadDword(key, L"Disable", 0);
        WdfRegistryClose(key);
    }
    if (disable) return STATUS_SUCCESS;

    P->Applied = 0;
    for (i = 0; i < RTL_NUMBER_OF(gPinmuxTable); i++) {
        const PINMUX_ENTRY *e = &gPinmuxTable[i];
        if (!(mask & (1UL << e->Group)) || !OffsetOk(e->Offset)) continue;
        if (Rmw(P, e->Offset, e->Clear, e->Set)) P->Applied++;
    }
    return STATUS_SUCCESS;
}

static VOID OnIoctl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutLen, size_t InLen, ULONG Code)
{
    PGOYA_PINMUX P = GetPm(WdfIoQueueGetDevice(Queue));
    PINMUX_REQ *in;
    PULONG out;
    size_t got;
    NTSTATUS st;
    UNREFERENCED_PARAMETER(OutLen);
    UNREFERENCED_PARAMETER(InLen);

    if (!P->Base) { WdfRequestComplete(Request, STATUS_DEVICE_NOT_READY); return; }

    st = WdfRequestRetrieveInputBuffer(Request, sizeof(PINMUX_REQ), (PVOID *)&in, &got);
    if (!NT_SUCCESS(st)) { WdfRequestComplete(Request, st); return; }
    if (!OffsetOk(in->Offset)) { WdfRequestComplete(Request, STATUS_INVALID_PARAMETER); return; }

    switch (Code) {
    case IOCTL_GOYAPINMUX_GET:
        st = WdfRequestRetrieveOutputBuffer(Request, sizeof(ULONG), (PVOID *)&out, &got);
        if (!NT_SUCCESS(st)) break;
        *out = READ_REGISTER_ULONG((PULONG)(P->Base + in->Offset));
        WdfRequestSetInformation(Request, sizeof(ULONG));
        break;
    case IOCTL_GOYAPINMUX_SET:
        Rmw(P, in->Offset, in->Clear, in->Set);
        st = STATUS_SUCCESS;
        break;
    default:
        st = STATUS_INVALID_DEVICE_REQUEST;
    }
    WdfRequestComplete(Request, st);
}

static NTSTATUS OnDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT Init)
{
    NTSTATUS st;
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES a;
    WDF_IO_QUEUE_CONFIG qc;
    WDFDEVICE dev;
    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = OnPrepareHardware;
    pnp.EvtDeviceReleaseHardware = OnReleaseHardware;
    pnp.EvtDeviceD0Entry = OnD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(Init, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a, GOYA_PINMUX);
    st = WdfDeviceCreate(&Init, &a, &dev);
    if (!NT_SUCCESS(st)) return st;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qc, WdfIoQueueDispatchParallel);
    qc.EvtIoDeviceControl = OnIoctl;
    st = WdfIoQueueCreate(dev, &qc, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(st)) return st;

    return WdfDeviceCreateDeviceInterface(dev, &GUID_DEVINTERFACE_GOYAPINMUX, NULL);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG cfg;
    WDF_DRIVER_CONFIG_INIT(&cfg, OnDeviceAdd);
    cfg.DriverPoolTag = PM_TAG;
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
}
