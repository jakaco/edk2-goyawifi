/* Minimal helper: talk to one I2C slave through the Resource Hub / SpbCx I/O target described by an
   ACPI I2cSerialBus() resource of our own device. Open lazily (the controller may start after us). */
#pragma once
#include <ntddk.h>
#include <wdf.h>
#include <reshub.h>

typedef struct _SPB_TGT {
    LARGE_INTEGER ConnId;
    BOOLEAN       HaveId;
    WDFIOTARGET   Target;
    BOOLEAN       Opened;
} SPB_TGT, *PSPB_TGT;

/* Look for the I2C connection resource in the translated list. */
static BOOLEAN SpbTgtFindResource(WDFCMRESLIST Translated, PSPB_TGT T)
{
    ULONG n = WdfCmResourceListGetCount(Translated), i;
    for (i = 0; i < n; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = WdfCmResourceListGetDescriptor(Translated, i);
        if (r->Type == CmResourceTypeConnection &&
            r->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
            r->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C) {
            T->ConnId.LowPart = r->u.Connection.IdLowPart;
            T->ConnId.HighPart = (LONG)r->u.Connection.IdHighPart;
            T->HaveId = TRUE;
            return TRUE;
        }
    }
    return FALSE;
}

/* PASSIVE_LEVEL. Returns TRUE when the target is open. */
static BOOLEAN SpbTgtOpen(WDFDEVICE Device, PSPB_TGT T)
{
    DECLARE_UNICODE_STRING_SIZE(path, RESOURCE_HUB_PATH_SIZE);
    WDF_IO_TARGET_OPEN_PARAMS p;
    NTSTATUS st;

    if (T->Opened) return TRUE;
    if (!T->HaveId) return FALSE;
    if (T->Target == NULL) {
        WDF_OBJECT_ATTRIBUTES a;
        WDF_OBJECT_ATTRIBUTES_INIT(&a);
        a.ParentObject = Device;
        st = WdfIoTargetCreate(Device, &a, &T->Target);
        if (!NT_SUCCESS(st)) return FALSE;
    }
    st = RESOURCE_HUB_CREATE_PATH_FROM_ID(&path, T->ConnId.LowPart, T->ConnId.HighPart);
    if (!NT_SUCCESS(st)) return FALSE;
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&p, &path, GENERIC_READ | GENERIC_WRITE);
    st = WdfIoTargetOpen(T->Target, &p);
    if (!NT_SUCCESS(st)) return FALSE;
    T->Opened = TRUE;
    return TRUE;
}

static VOID SpbTgtClose(PSPB_TGT T)
{
    if (T->Opened) { WdfIoTargetClose(T->Target); T->Opened = FALSE; }
}

static NTSTATUS SpbTgtWrite(PSPB_TGT T, PVOID Buf, ULONG Len)
{
    WDF_MEMORY_DESCRIPTOR m;
    ULONG_PTR done = 0;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&m, Buf, Len);
    return WdfIoTargetSendWriteSynchronously(T->Target, NULL, &m, NULL, NULL, &done);
}

static NTSTATUS SpbTgtRead(PSPB_TGT T, PVOID Buf, ULONG Len)
{
    WDF_MEMORY_DESCRIPTOR m;
    ULONG_PTR done = 0;
    NTSTATUS st;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&m, Buf, Len);
    st = WdfIoTargetSendReadSynchronously(T->Target, NULL, &m, NULL, NULL, &done);
    if (NT_SUCCESS(st) && done != Len) st = STATUS_DEVICE_PROTOCOL_ERROR;
    return st;
}

static VOID SleepMs(ULONG Ms)
{
    LARGE_INTEGER t;
    t.QuadPart = -10000LL * (LONGLONG)Ms;
    KeDelayExecutionThread(KernelMode, FALSE, &t);
}
