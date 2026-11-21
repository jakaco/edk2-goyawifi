/* Shared helpers for the Goya KMDF HID source drivers (goyatouch, goyakeys).

   Model (same as the WDK vhidmini2 sample, KMDF flavour): the in-box mshidkmdf.sys is the function
   driver and registers with hidclass; our KMDF driver is a *lower filter* (WdfFdoInitSetFilter) that
   answers the IOCTL_HID_* internal requests. Input reports are queued by the driver and handed to
   pending IOCTL_HID_READ_REPORT requests.

   Everything here is header-only (static) so each driver stays one .c file for build-kmdf.cmd. */
#pragma once
#include <ntddk.h>
#include <wdf.h>
#include <hidport.h>
#include "goyatrace.h"

#define HIDMINI_RING 8
#define HIDMINI_MAX_REPORT 40

typedef NTSTATUS (*HIDMINI_GET_FEATURE)(PVOID Ctx, UCHAR ReportId, PUCHAR Buf, ULONG Len, PULONG Written);

typedef struct _HIDMINI_CONFIG {
    const UCHAR *ReportDescriptor;
    ULONG        ReportDescriptorLength;
    USHORT       VendorId, ProductId, VersionNumber;
    HIDMINI_GET_FEATURE GetFeature;     /* optional */
    PVOID        Ctx;
} HIDMINI_CONFIG;

typedef struct _HIDMINI_CTX {
    HIDMINI_CONFIG Cfg;
    WDFQUEUE       ReadQueue;           /* manual queue of pending READ_REPORT requests */
    WDFSPINLOCK    Lock;
    UCHAR          Ring[HIDMINI_RING][HIDMINI_MAX_REPORT];
    ULONG          RingLen[HIDMINI_RING];
    ULONG          Head, Count;
} HIDMINI_CTX, *PHIDMINI_CTX;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(HIDMINI_CTX, HidMiniCtx)

/* Hand queued reports to waiting READ_REPORT requests. */
static VOID HidMiniDeliver(WDFDEVICE Device)
{
    PHIDMINI_CTX c = HidMiniCtx(Device);
    for (;;) {
        WDFREQUEST req = NULL;
        UCHAR local[HIDMINI_MAX_REPORT];
        ULONG len = 0;
        NTSTATUS st;
        PVOID out;

        WdfSpinLockAcquire(c->Lock);
        if (c->Count == 0 || !NT_SUCCESS(WdfIoQueueRetrieveNextRequest(c->ReadQueue, &req))) {
            WdfSpinLockRelease(c->Lock);
            return;
        }
        len = c->RingLen[c->Head];
        RtlCopyMemory(local, c->Ring[c->Head], len);
        c->Head = (c->Head + 1) % HIDMINI_RING;
        c->Count--;
        WdfSpinLockRelease(c->Lock);

        st = WdfRequestRetrieveOutputBuffer(req, len, &out, NULL);
        if (NT_SUCCESS(st)) {
            RtlCopyMemory(out, local, len);
            WdfRequestCompleteWithInformation(req, STATUS_SUCCESS, len);
        } else {
            WdfRequestComplete(req, st);
        }
    }
}

/* Queue one input report (first byte = report ID) and deliver it. Safe at <= DISPATCH_LEVEL. */
static VOID HidMiniReport(WDFDEVICE Device, const UCHAR *Report, ULONG Len)
{
    PHIDMINI_CTX c = HidMiniCtx(Device);
    ULONG slot;
    if (Len == 0 || Len > HIDMINI_MAX_REPORT) return;
    WdfSpinLockAcquire(c->Lock);
    if (c->Count == HIDMINI_RING) {                 /* ring full: drop the oldest */
        c->Head = (c->Head + 1) % HIDMINI_RING;
        c->Count--;
    }
    slot = (c->Head + c->Count) % HIDMINI_RING;
    RtlCopyMemory(c->Ring[slot], Report, Len);
    c->RingLen[slot] = Len;
    c->Count++;
    WdfSpinLockRelease(c->Lock);
    HidMiniDeliver(Device);
}

static VOID HidMiniIoInternal_(WDFQUEUE Queue, WDFREQUEST Request, size_t OutLen, size_t InLen, ULONG Code)
{
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    PHIDMINI_CTX c = HidMiniCtx(device);
    NTSTATUS st = STATUS_NOT_SUPPORTED;
    size_t info = 0;

    switch (Code) {
    case IOCTL_HID_GET_DEVICE_DESCRIPTOR: {
        HID_DESCRIPTOR d;
        WDFMEMORY mem;
        RtlZeroMemory(&d, sizeof(d));
        d.bLength = 0x09;
        d.bDescriptorType = 0x21;
        d.bcdHID = 0x0100;
        d.bCountry = 0;
        d.bNumDescriptors = 1;
        d.DescriptorList[0].bReportType = 0x22;
        d.DescriptorList[0].wReportLength = (USHORT)c->Cfg.ReportDescriptorLength;
        if (OutLen < d.bLength) { st = STATUS_BUFFER_TOO_SMALL; break; }
        st = WdfRequestRetrieveOutputMemory(Request, &mem);
        if (NT_SUCCESS(st)) st = WdfMemoryCopyFromBuffer(mem, 0, &d, d.bLength);
        if (NT_SUCCESS(st)) info = d.bLength;
        break;
    }
    case IOCTL_HID_GET_REPORT_DESCRIPTOR: {
        WDFMEMORY mem;
        if (OutLen < c->Cfg.ReportDescriptorLength) { st = STATUS_BUFFER_TOO_SMALL; break; }
        st = WdfRequestRetrieveOutputMemory(Request, &mem);
        if (NT_SUCCESS(st)) st = WdfMemoryCopyFromBuffer(mem, 0, (PVOID)c->Cfg.ReportDescriptor, c->Cfg.ReportDescriptorLength);
        if (NT_SUCCESS(st)) info = c->Cfg.ReportDescriptorLength;
        break;
    }
    case IOCTL_HID_GET_DEVICE_ATTRIBUTES: {
        PHID_DEVICE_ATTRIBUTES a;
        st = WdfRequestRetrieveOutputBuffer(Request, sizeof(HID_DEVICE_ATTRIBUTES), (PVOID *)&a, NULL);
        if (NT_SUCCESS(st)) {
            RtlZeroMemory(a, sizeof(*a));
            a->Size = sizeof(HID_DEVICE_ATTRIBUTES);
            a->VendorID = c->Cfg.VendorId;
            a->ProductID = c->Cfg.ProductId;
            a->VersionNumber = c->Cfg.VersionNumber;
            info = sizeof(*a);
        }
        break;
    }
    case IOCTL_HID_READ_REPORT:
        st = WdfRequestForwardToIoQueue(Request, c->ReadQueue);
        if (NT_SUCCESS(st)) { HidMiniDeliver(device); return; }
        break;
    case IOCTL_HID_GET_FEATURE: {
        /* Two delivery models exist: hidclass's own METHOD_NEITHER request passed straight through
           (the 8520 mshidkmdf.sys is a plain pass-through: HID_XFER_PACKET in Irp->UserBuffer, no
           input buffer), or a converted request (report ID in the input buffer, report in the output
           buffer). Tell them apart by the input length and never dereference anything else. */
        PIRP irp = WdfRequestWdmGetIrp(Request);
        ULONG written = 0;
        if (c->Cfg.GetFeature == NULL) { st = STATUS_NOT_SUPPORTED; break; }
        if (InLen == 0 && OutLen >= sizeof(HID_XFER_PACKET) && irp->UserBuffer != NULL) {
            PHID_XFER_PACKET p = (PHID_XFER_PACKET)irp->UserBuffer;
            if (p->reportBuffer == NULL || p->reportBufferLen == 0) { st = STATUS_INVALID_BUFFER_SIZE; break; }
            st = c->Cfg.GetFeature(c->Cfg.Ctx, p->reportId, p->reportBuffer, p->reportBufferLen, &written);
        } else {
            WDFMEMORY inMem, outMem;
            size_t inSz = 0, outSz = 0;
            PUCHAR inBuf, outBuf;
            st = WdfRequestRetrieveInputMemory(Request, &inMem);
            if (NT_SUCCESS(st)) st = WdfRequestRetrieveOutputMemory(Request, &outMem);
            if (!NT_SUCCESS(st)) break;
            inBuf = (PUCHAR)WdfMemoryGetBuffer(inMem, &inSz);
            outBuf = (PUCHAR)WdfMemoryGetBuffer(outMem, &outSz);
            if (inBuf == NULL || inSz < 1 || outBuf == NULL || outSz == 0 || outSz > MAXULONG) { st = STATUS_INVALID_BUFFER_SIZE; break; }
            st = c->Cfg.GetFeature(c->Cfg.Ctx, inBuf[0], outBuf, (ULONG)outSz, &written);
        }
        if (NT_SUCCESS(st)) info = written;
        break;
    }
    case IOCTL_HID_SET_FEATURE:
    case IOCTL_HID_WRITE_REPORT:
    case IOCTL_HID_ACTIVATE_DEVICE:
    case IOCTL_HID_DEACTIVATE_DEVICE:
        st = STATUS_SUCCESS;
        break;
    default:
        st = STATUS_NOT_SUPPORTED;
        break;
    }
    WdfRequestCompleteWithInformation(Request, st, info);
}

/* Guarded: an exception leaves the request pending (it may already have been completed) */
static VOID HidMiniIoInternal(WDFQUEUE Queue, WDFREQUEST Request, size_t OutLen, size_t InLen, ULONG Code)
{
    GT_GUARD(0x20 + (Code >> 2 & 0x1F), HidMiniIoInternal_(Queue, Request, OutLen, InLen, Code), ;);
}

/* Call from EvtDriverDeviceAdd after WdfFdoInitSetFilter + WdfDeviceCreate. */
static NTSTATUS HidMiniCreate(WDFDEVICE Device, const HIDMINI_CONFIG *Cfg)
{
    NTSTATUS st;
    WDF_OBJECT_ATTRIBUTES a;
    PHIDMINI_CTX c;
    WDF_IO_QUEUE_CONFIG q;
    WDFQUEUE dq;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a, HIDMINI_CTX);
    st = WdfObjectAllocateContext(Device, &a, (PVOID *)&c);
    if (!NT_SUCCESS(st)) return st;
    c->Cfg = *Cfg;

    WDF_OBJECT_ATTRIBUTES_INIT(&a);
    a.ParentObject = Device;
    st = WdfSpinLockCreate(&a, &c->Lock);
    if (!NT_SUCCESS(st)) return st;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&q, WdfIoQueueDispatchParallel);
    q.EvtIoInternalDeviceControl = HidMiniIoInternal;
    st = WdfIoQueueCreate(Device, &q, WDF_NO_OBJECT_ATTRIBUTES, &dq);
    if (!NT_SUCCESS(st)) return st;

    WDF_IO_QUEUE_CONFIG_INIT(&q, WdfIoQueueDispatchManual);
    return WdfIoQueueCreate(Device, &q, WDF_NO_OBJECT_ATTRIBUTES, &c->ReadQueue);
}
