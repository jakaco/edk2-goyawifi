/*
 * goyasdhc - microSD boot driver for Windows 8.0 RT on the Galaxy Tab 3 Lite (PXA986/988, SDH1).
 *
 * Windows 8.0 has no sdport.sys (the SD miniport model is Windows 10 only) and its inbox sdbus.sys
 * cannot cope with this controller's quirks, so this is a Storport miniport that exposes the card as
 * one fixed SCSI disk (LUN 0) and runs the SD protocol itself (sdcore.c):
 *   - polled PIO only: no interrupt, no DMA (works the same in normal, crash-dump and hibernate mode),
 *   - 52 MHz base clock (capability register is not trusted), 3.3 V only,
 *     high-speed 26 MHz with automatic fallback to 13 / 6.5 MHz and 1-bit on errors,
 *   - card-detect override in Host Control 1 (the slot's detect line is GPIO 11, not the CD pin),
 *   - optional second memory range = APMU SDH0/SDH1 clock registers; the clock is re-enabled if the
 *     firmware left it off.
 * Requests are executed synchronously in HwStartIo (Storport serialises StartIo calls).
 *
 * ACPI: \_SB.SDH1, HID MRVL0007, Memory32Fixed 0xD4280000/0x1000 [+ 0xD4282854/0x8], no interrupt.
 */
#include <miniport.h>
#include <storport.h>
#include <string.h>

#ifndef RtlZeroMemory
#define RtlZeroMemory(d, l)    memset((d), 0, (l))
#endif
#ifndef RtlCopyMemory
#define RtlCopyMemory(d, s, l) memcpy((d), (s), (l))
#endif
#define GOYA_COUNT(a) (sizeof(a) / sizeof((a)[0]))

#ifndef STOR_MAP_ALL_BUFFERS_INCLUDING_READ_WRITE
#define STOR_MAP_ALL_BUFFERS_INCLUDING_READ_WRITE 3
#endif

#define SD_RD8(c, o)      StorPortReadRegisterUchar((c)->Os, (PUCHAR)((c)->Regs + (o)))
#define SD_RD16(c, o)     StorPortReadRegisterUshort((c)->Os, (PUSHORT)((c)->Regs + (o)))
#define SD_RD32(c, o)     StorPortReadRegisterUlong((c)->Os, (PULONG)((c)->Regs + (o)))
#define SD_WR8(c, o, v)   StorPortWriteRegisterUchar((c)->Os, (PUCHAR)((c)->Regs + (o)), (UCHAR)(v))
#define SD_WR16(c, o, v)  StorPortWriteRegisterUshort((c)->Os, (PUSHORT)((c)->Regs + (o)), (USHORT)(v))
#define SD_WR32(c, o, v)  StorPortWriteRegisterUlong((c)->Os, (PULONG)((c)->Regs + (o)), (ULONG)(v))
#define SD_STALL(us)      StorPortStallExecution(us)
#define SD_LOG(...)       StorPortDebugPrint(0, __VA_ARGS__)

#include "sdcore.c"

/* APMU SDH clock control (offsets inside the optional 0xD4282854 range) */
#define APMU_SDH0_OFF     0x0
#define APMU_SDH1_OFF     0x4
#define SDH_CLK_VALUE     (0x0013 | (7 << 8) | 0x0800)
#define SDH_CLK_ACLK_EN   0x0008

/* SCSI */
#define OP_TEST_UNIT_READY   0x00
#define OP_REQUEST_SENSE     0x03
#define OP_READ6             0x08
#define OP_WRITE6            0x0A
#define OP_INQUIRY           0x12
#define OP_MODE_SELECT6      0x15
#define OP_MODE_SENSE6       0x1A
#define OP_START_STOP        0x1B
#define OP_PREVENT_ALLOW     0x1E
#define OP_READ_CAPACITY     0x25
#define OP_READ10            0x28
#define OP_WRITE10           0x2A
#define OP_VERIFY10          0x2F
#define OP_SYNC_CACHE10      0x35
#define OP_MODE_SELECT10     0x55
#define OP_MODE_SENSE10      0x5A
#define OP_READ16            0x88
#define OP_WRITE16           0x8A
#define OP_VERIFY16          0x8F
#define OP_SYNC_CACHE16      0x91
#define OP_SAI16             0x9E
#define OP_REPORT_LUNS       0xA0
#define OP_READ12            0xA8
#define OP_WRITE12           0xAA

#define SCSI_CHECK_CONDITION 0x02
#define KEY_NOT_READY        0x02
#define KEY_MEDIUM_ERROR     0x03
#define KEY_HW_ERROR         0x04
#define KEY_ILLEGAL_REQUEST  0x05

#define MAX_XFER_BLOCKS      128        /* 64 KiB per request */

typedef struct _GOYA_EXT {
    SD_HOST  Sd;
    PUCHAR   Apmu;
    BOOLEAN  InitDone;
    UCHAR    Sense[3];                  /* key, asc, ascq of the last error */
} GOYA_EXT, *PGOYA_EXT;

/* The microSD controller (0xD4280000, Linux "sdh1") is clocked by APMU_SDH0 (vendor DT:
   assigned-clocks = PXA988_CLK_SDH0), which also holds the shared AXI clock enable. */
static VOID GoyaClocks(PGOYA_EXT X)
{
    ULONG v;
    if (!X->Apmu) return;
    v = StorPortReadRegisterUlong(X, (PULONG)(X->Apmu + APMU_SDH0_OFF));
    if ((v & 0x0013) != 0x0013) {       /* off: same setting as the firmware (Pxa988SdhciDxe) */
        StorPortWriteRegisterUlong(X, (PULONG)(X->Apmu + APMU_SDH0_OFF), SDH_CLK_VALUE | SDH_CLK_ACLK_EN);
        StorPortStallExecution(20);
    } else if (!(v & SDH_CLK_ACLK_EN)) {
        StorPortWriteRegisterUlong(X, (PULONG)(X->Apmu + APMU_SDH0_OFF), v | SDH_CLK_ACLK_EN);
    }
    v = StorPortReadRegisterUlong(X, (PULONG)(X->Apmu + APMU_SDH1_OFF));
    if ((v & 0x0013) != 0x0013) {
        StorPortWriteRegisterUlong(X, (PULONG)(X->Apmu + APMU_SDH1_OFF), SDH_CLK_VALUE);
        StorPortStallExecution(20);
    }
}

static BOOLEAN GoyaInitCard(PGOYA_EXT X)
{
    ULONG tries;
    GoyaClocks(X);
    for (tries = 0; tries < 4; tries++) {
        if (SdInitCard(&X->Sd) == SD_OK) return TRUE;
        if (X->Sd.SpeedLevel < 3) X->Sd.SpeedLevel++;
    }
    return FALSE;
}

/* ---- Storport entry points ---- */

static ULONG GoyaFindAdapter(PVOID Ext, PVOID HwContext, PVOID BusInfo, PCHAR Args,
                             PPORT_CONFIGURATION_INFORMATION Cfg, PBOOLEAN Again)
{
    PGOYA_EXT X = (PGOYA_EXT)Ext;
    ULONG i;
    UNREFERENCED_PARAMETER(HwContext); UNREFERENCED_PARAMETER(BusInfo);
    UNREFERENCED_PARAMETER(Args); UNREFERENCED_PARAMETER(Again);

    RtlZeroMemory(X, sizeof(*X));
    X->Sd.Os = X;
    for (i = 0; i < Cfg->NumberOfAccessRanges; i++) {
        PACCESS_RANGE r = &(*Cfg->AccessRanges)[i];
        if (r->RangeLength == 0) continue;
        if (!X->Sd.Regs && r->RangeLength >= 0x100) {
            X->Sd.Regs = (volatile UCHAR *)StorPortGetDeviceBase(X, Cfg->AdapterInterfaceType,
                Cfg->SystemIoBusNumber, r->RangeStart, r->RangeLength, (BOOLEAN)!r->RangeInMemory);
        } else if (!X->Apmu && r->RangeLength >= 8) {
            X->Apmu = (PUCHAR)StorPortGetDeviceBase(X, Cfg->AdapterInterfaceType,
                Cfg->SystemIoBusNumber, r->RangeStart, r->RangeLength, (BOOLEAN)!r->RangeInMemory);
        }
    }
    if (!X->Sd.Regs) return SP_RETURN_BAD_CONFIG;

    /* make sure a stale firmware setting can never raise an interrupt */
    SD_WR32(&X->Sd, SDHC_SIGEN, 0);
    SD_WR32(&X->Sd, SDHC_INTSTS, 0xFFFFFFFF);

    Cfg->MaximumTransferLength = MAX_XFER_BLOCKS * SD_BLOCK;
    Cfg->NumberOfPhysicalBreaks = MAX_XFER_BLOCKS + 1;
    Cfg->AlignmentMask = 0;
    Cfg->NumberOfBuses = 1;
    Cfg->MaximumNumberOfTargets = 1;
    Cfg->MaximumNumberOfLogicalUnits = 1;
    Cfg->ScatterGather = TRUE;
    Cfg->Master = TRUE;
    Cfg->CachesData = FALSE;
    Cfg->MapBuffers = STOR_MAP_ALL_BUFFERS_INCLUDING_READ_WRITE;
    Cfg->InitiatorBusId[0] = 7;
    return SP_RETURN_FOUND;
}

static BOOLEAN GoyaPassiveInit(PVOID Ext)
{
    PGOYA_EXT X = (PGOYA_EXT)Ext;
    X->InitDone = TRUE;
    if (!GoyaInitCard(X)) StorPortDebugPrint(0, "goyasdhc: card initialisation failed\n");
    return TRUE;                        /* keep the adapter; I/O will retry the card init */
}

static BOOLEAN GoyaInitialize(PVOID Ext)
{
    /* Card init can take ~1 s: do it at PASSIVE_LEVEL when allowed (not in dump mode). */
    if (StorPortEnablePassiveInitialization(Ext, GoyaPassiveInit)) return TRUE;
    return GoyaPassiveInit(Ext);
}

static BOOLEAN GoyaInterrupt(PVOID Ext)
{
    UNREFERENCED_PARAMETER(Ext);
    return FALSE;                       /* polled; signal enables are always 0 */
}

static BOOLEAN GoyaResetBus(PVOID Ext, ULONG PathId)
{
    PGOYA_EXT X = (PGOYA_EXT)Ext;
    X->Sd.Ready = FALSE;
    GoyaInitCard(X);
    StorPortCompleteRequest(Ext, (UCHAR)PathId, SP_UNTAGGED, SP_UNTAGGED, SRB_STATUS_BUS_RESET);
    return TRUE;
}

static SCSI_ADAPTER_CONTROL_STATUS GoyaAdapterControl(PVOID Ext, SCSI_ADAPTER_CONTROL_TYPE Type, PVOID Param)
{
    PGOYA_EXT X = (PGOYA_EXT)Ext;
    switch (Type) {
    case ScsiQuerySupportedControlTypes: {
        PSCSI_SUPPORTED_CONTROL_TYPE_LIST l = (PSCSI_SUPPORTED_CONTROL_TYPE_LIST)Param;
        if (l->MaxControlType > ScsiQuerySupportedControlTypes) l->SupportedTypeList[ScsiQuerySupportedControlTypes] = TRUE;
        if (l->MaxControlType > ScsiStopAdapter) l->SupportedTypeList[ScsiStopAdapter] = TRUE;
        if (l->MaxControlType > ScsiRestartAdapter) l->SupportedTypeList[ScsiRestartAdapter] = TRUE;
        return ScsiAdapterControlSuccess;
    }
    case ScsiStopAdapter:
        return ScsiAdapterControlSuccess;
    case ScsiRestartAdapter:            /* resume: the controller may have lost its state */
        X->Sd.Ready = FALSE;
        GoyaInitCard(X);
        return ScsiAdapterControlSuccess;
    default:
        return ScsiAdapterControlUnsuccessful;
    }
}

static VOID SetSense(PSCSI_REQUEST_BLOCK Srb, PGOYA_EXT X, UCHAR Key, UCHAR Asc, UCHAR Ascq)
{
    PUCHAR s = (PUCHAR)Srb->SenseInfoBuffer;
    X->Sense[0] = Key; X->Sense[1] = Asc; X->Sense[2] = Ascq;
    Srb->ScsiStatus = SCSI_CHECK_CONDITION;
    Srb->SrbStatus = SRB_STATUS_ERROR;
    if (s && Srb->SenseInfoBufferLength >= 14) {
        RtlZeroMemory(s, Srb->SenseInfoBufferLength);
        s[0] = 0x70; s[2] = Key; s[7] = 10; s[12] = Asc; s[13] = Ascq;
        Srb->SrbStatus |= SRB_STATUS_AUTOSENSE_VALID;
    }
}

static VOID ReturnData(PSCSI_REQUEST_BLOCK Srb, const VOID *Data, ULONG Len)
{
    if (Len > Srb->DataTransferLength) Len = Srb->DataTransferLength;
    if (Len && Srb->DataBuffer) RtlCopyMemory(Srb->DataBuffer, Data, Len);
    Srb->DataTransferLength = Len;
    Srb->SrbStatus = SRB_STATUS_SUCCESS;
}

static ULONG Be32(const UCHAR *p) { return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | p[3]; }
static VOID  PutBe32(PUCHAR p, ULONG v) { p[0] = (UCHAR)(v >> 24); p[1] = (UCHAR)(v >> 16); p[2] = (UCHAR)(v >> 8); p[3] = (UCHAR)v; }

static BOOLEAN EnsureCard(PGOYA_EXT X)
{
    if (!X->Sd.Ready) GoyaInitCard(X);
    return X->Sd.Ready;
}

static VOID DoReadWrite(PGOYA_EXT X, PSCSI_REQUEST_BLOCK Srb, BOOLEAN Write)
{
    const UCHAR *cdb = Srb->Cdb;
    ULONGLONG lba;
    ULONG n, done;
    SD_STATUS st;

    switch (cdb[0]) {
    case OP_READ6: case OP_WRITE6:
        lba = ((ULONG)(cdb[1] & 0x1F) << 16) | ((ULONG)cdb[2] << 8) | cdb[3];
        n = cdb[4] ? cdb[4] : 256; break;
    case OP_READ10: case OP_WRITE10:
        lba = Be32(cdb + 2); n = ((ULONG)cdb[7] << 8) | cdb[8]; break;
    case OP_READ12: case OP_WRITE12:
        lba = Be32(cdb + 2); n = Be32(cdb + 6); break;
    default:
        lba = ((ULONGLONG)Be32(cdb + 2) << 32) | Be32(cdb + 6); n = Be32(cdb + 10); break;
    }

    if (n == 0) { Srb->SrbStatus = SRB_STATUS_SUCCESS; return; }
    if (!EnsureCard(X)) { SetSense(Srb, X, KEY_NOT_READY, 0x3A, 0); return; }
    if (lba + n > X->Sd.Blocks) { SetSense(Srb, X, KEY_ILLEGAL_REQUEST, 0x21, 0); return; }
    if ((ULONGLONG)n * SD_BLOCK > Srb->DataTransferLength || !Srb->DataBuffer) {
        SetSense(Srb, X, KEY_ILLEGAL_REQUEST, 0x24, 0); return;
    }

    done = 0;
    while (n) {
        ULONG chunk = n > MAX_XFER_BLOCKS ? MAX_XFER_BLOCKS : n;
        PUCHAR p = (PUCHAR)Srb->DataBuffer + done * SD_BLOCK;
        st = SdTransfer(&X->Sd, (BOOLEAN)!Write, lba, chunk, p);
        done += chunk;
        if (st != SD_OK) {
            SetSense(Srb, X, st == SD_ETIMEOUT || st == SD_ENOCARD ? KEY_HW_ERROR : KEY_MEDIUM_ERROR,
                     Write ? 0x0C : 0x11, 0);
            return;
        }
        lba += chunk; n -= chunk;
    }
    Srb->SrbStatus = SRB_STATUS_SUCCESS;
}

static VOID DoInquiry(PGOYA_EXT X, PSCSI_REQUEST_BLOCK Srb)
{
    UCHAR d[96];
    RtlZeroMemory(d, sizeof(d));
    if (Srb->Cdb[1] & 1) {                       /* VPD */
        switch (Srb->Cdb[2]) {
        case 0x00:
            d[1] = 0x00; d[3] = 2; d[4] = 0x00; d[5] = 0x80;
            ReturnData(Srb, d, 6); return;
        case 0x80: {                             /* unit serial = CID product serial number */
            ULONG psn = SdCsdBits(X->Sd.Cid, 24, 32), i;
            d[1] = 0x80; d[3] = 8;
            for (i = 0; i < 8; i++) d[4 + i] = "0123456789ABCDEF"[(psn >> (28 - 4 * i)) & 0xF];
            ReturnData(Srb, d, 12); return;
        }
        default:
            SetSense(Srb, X, KEY_ILLEGAL_REQUEST, 0x24, 0); return;
        }
    }
    d[0] = 0x00;                                 /* direct access block device */
    d[1] = 0x00;                                 /* not removable: Windows will not boot from removable disks */
    d[2] = 0x05; d[3] = 0x02; d[4] = 31;
    RtlCopyMemory(d + 8,  "GOYA    ", 8);
    RtlCopyMemory(d + 16, "microSD boot    ", 16);
    RtlCopyMemory(d + 32, "1.0 ", 4);
    ReturnData(Srb, d, 36);
}

static VOID DoModeSense(PGOYA_EXT X, PSCSI_REQUEST_BLOCK Srb, BOOLEAN Ten)
{
    UCHAR d[64], page = Srb->Cdb[2] & 0x3F;
    ULONG h = Ten ? 8 : 4, len = h;
    UNREFERENCED_PARAMETER(X);
    RtlZeroMemory(d, sizeof(d));
    if (page == 0x08 || page == 0x3F) {          /* caching page: no write cache */
        d[h] = 0x08; d[h + 1] = 0x12;
        len += 20;
    }
    if (Ten) { d[0] = (UCHAR)((len - 2) >> 8); d[1] = (UCHAR)(len - 2); }
    else d[0] = (UCHAR)(len - 1);
    ReturnData(Srb, d, len);
}

static VOID DoScsi(PGOYA_EXT X, PSCSI_REQUEST_BLOCK Srb)
{
    UCHAR d[32];
    RtlZeroMemory(d, sizeof(d));
    switch (Srb->Cdb[0]) {
    case OP_TEST_UNIT_READY:
        if (EnsureCard(X)) Srb->SrbStatus = SRB_STATUS_SUCCESS;
        else SetSense(Srb, X, KEY_NOT_READY, 0x3A, 0);
        break;
    case OP_REQUEST_SENSE:
        d[0] = 0x70; d[2] = X->Sense[0]; d[7] = 10; d[12] = X->Sense[1]; d[13] = X->Sense[2];
        X->Sense[0] = X->Sense[1] = X->Sense[2] = 0;
        ReturnData(Srb, d, 18);
        break;
    case OP_INQUIRY:
        DoInquiry(X, Srb);
        break;
    case OP_READ_CAPACITY:
        if (!EnsureCard(X)) { SetSense(Srb, X, KEY_NOT_READY, 0x3A, 0); break; }
        PutBe32(d, X->Sd.Blocks - 1 > 0xFFFFFFFFULL ? 0xFFFFFFFFUL : (ULONG)(X->Sd.Blocks - 1));
        PutBe32(d + 4, SD_BLOCK);
        ReturnData(Srb, d, 8);
        break;
    case OP_SAI16:
        if ((Srb->Cdb[1] & 0x1F) != 0x10) { SetSense(Srb, X, KEY_ILLEGAL_REQUEST, 0x20, 0); break; }
        if (!EnsureCard(X)) { SetSense(Srb, X, KEY_NOT_READY, 0x3A, 0); break; }
        PutBe32(d, (ULONG)((X->Sd.Blocks - 1) >> 32));
        PutBe32(d + 4, (ULONG)(X->Sd.Blocks - 1));
        PutBe32(d + 8, SD_BLOCK);
        ReturnData(Srb, d, 32);
        break;
    case OP_READ6: case OP_READ10: case OP_READ12: case OP_READ16:
        DoReadWrite(X, Srb, FALSE);
        break;
    case OP_WRITE6: case OP_WRITE10: case OP_WRITE12: case OP_WRITE16:
        DoReadWrite(X, Srb, TRUE);
        break;
    case OP_MODE_SENSE6:  DoModeSense(X, Srb, FALSE); break;
    case OP_MODE_SENSE10: DoModeSense(X, Srb, TRUE);  break;
    case OP_REPORT_LUNS:
        d[3] = 8;                                /* one LUN, LUN 0 */
        ReturnData(Srb, d, 16);
        break;
    case OP_START_STOP: case OP_PREVENT_ALLOW: case OP_VERIFY10: case OP_VERIFY16:
    case OP_SYNC_CACHE10: case OP_SYNC_CACHE16: case OP_MODE_SELECT6: case OP_MODE_SELECT10:
        Srb->DataTransferLength = 0;
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        break;
    default:
        SetSense(Srb, X, KEY_ILLEGAL_REQUEST, 0x20, 0);
        break;
    }
}

static BOOLEAN GoyaStartIo(PVOID Ext, PSCSI_REQUEST_BLOCK Srb)
{
    PGOYA_EXT X = (PGOYA_EXT)Ext;

    switch (Srb->Function) {
    case SRB_FUNCTION_EXECUTE_SCSI:
        if (Srb->PathId != 0 || Srb->TargetId != 0) Srb->SrbStatus = SRB_STATUS_SELECTION_TIMEOUT;
        else if (Srb->Lun != 0) Srb->SrbStatus = SRB_STATUS_INVALID_LUN;
        else { Srb->ScsiStatus = 0; DoScsi(X, Srb); }
        break;
    case SRB_FUNCTION_RESET_BUS:
    case SRB_FUNCTION_RESET_DEVICE:
    case SRB_FUNCTION_RESET_LOGICAL_UNIT:
        X->Sd.Ready = FALSE;
        GoyaInitCard(X);
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        break;
    case SRB_FUNCTION_FLUSH:
    case SRB_FUNCTION_SHUTDOWN:
    case SRB_FUNCTION_PNP:
    case SRB_FUNCTION_POWER:
    case SRB_FUNCTION_ABORT_COMMAND:
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        break;
    default:
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        break;
    }
    StorPortNotification(RequestComplete, Ext, Srb);
    return TRUE;
}

ULONG DriverEntry(PVOID DriverObject, PVOID RegistryPath)
{
    static const INTERFACE_TYPE types[] = { Internal, ACPIBus, PNPBus, Isa };
    HW_INITIALIZATION_DATA h;
    ULONG i, st, best = 0xC0000001UL;   /* STATUS_UNSUCCESSFUL */

    /* The ACPI PDO's legacy bus type is not documented for ARM; register for every likely one. */
    for (i = 0; i < GOYA_COUNT(types); i++) {
        RtlZeroMemory(&h, sizeof(h));
        h.HwInitializationDataSize = sizeof(h);
        h.AdapterInterfaceType = types[i];
        h.HwInitialize = GoyaInitialize;
        h.HwStartIo = GoyaStartIo;
        h.HwInterrupt = GoyaInterrupt;
        h.HwFindAdapter = GoyaFindAdapter;
        h.HwResetBus = GoyaResetBus;
        h.HwAdapterControl = GoyaAdapterControl;
        h.DeviceExtensionSize = sizeof(GOYA_EXT);
        h.SpecificLuExtensionSize = 0;
        h.SrbExtensionSize = 0;
        h.NumberOfAccessRanges = 2;
        h.MapBuffers = STOR_MAP_ALL_BUFFERS_INCLUDING_READ_WRITE;
        h.NeedPhysicalAddresses = TRUE;
        h.TaggedQueuing = FALSE;
        h.AutoRequestSense = TRUE;
        h.MultipleRequestPerLu = FALSE;
        st = StorPortInitialize(DriverObject, RegistryPath, &h, NULL);
        if (st == 0 || best != 0) best = st;
    }
    return best;
}
