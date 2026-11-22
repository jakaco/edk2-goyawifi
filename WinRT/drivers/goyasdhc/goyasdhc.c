/*
 * goyasdhc - microSD boot driver for Windows 8.0 RT on the Galaxy Tab 3 Lite (PXA986/988, SDH1).
 *
 * Windows 8.0 has no sdport.sys (the SD miniport model is Windows 10 only) and its inbox sdbus.sys
 * cannot cope with this controller's quirks, so this is a Storport miniport that exposes the card as
 * one fixed SCSI disk (LUN 0) and runs the SD protocol itself (sdcore.c):
 *   - polled PIO only, no DMA (works the same in normal, crash-dump and hibernate mode). The adapter
 *     reports VirtualDevice so Storport neither connects an interrupt nor touches DMA (see FindAdapter).
 *     The DSDT still lists the SDH interrupt (GIC 71, shared by SDH1/SDH2/SDH3); every SDH signal
 *     enable is kept at 0, and the interrupt routine silences any controller that still signals,
 *   - 52 MHz base clock (capability register is not trusted), 3.3 V only,
 *     high-speed 26 MHz with automatic fallback to 13 / 6.5 MHz and 1-bit on errors,
 *   - card-detect override in Host Control 1 (the slot's detect line is GPIO 11, not the CD pin),
 *   - optional second memory range = APMU SDH0/SDH1 clock registers; the clock is re-enabled if the
 *     firmware left it off.
 * Requests are executed synchronously in HwStartIo (Storport serialises StartIo calls). StartIo runs at
 * DISPATCH_LEVEL, so a card that does not initialise is retried only a few times (then reported as
 * not ready until a bus reset), never on every request: a dead card must not stall the system.
 *
 * On-screen status (id 1, same layout as common/goyatrace.h, which cannot be used with the Storport
 * headers): top right "1 <stage>", below it four values:
 *   stages  00 DriverEntry  01 FindAdapter  02 adapter found  0E no register range  03 HwInitialize
 *           04 card init  05 card ready  0F card init failed  06 first request
 *           0A registers mapped by Storport  0B registers mapped by goyasdhc (no usable range)
 *   1 0  card: bits 31:28 speed level, 27 4-bit, 26 high speed, 25 SDHC/XC, 23:0 size in MB
 *   1 1  requests: bits 31:24 last SCSI opcode, 23:16 failed requests, 15:0 requests
 *   1 2  last error: bits 31:28 SD_STATUS, 27:0 SDHCI interrupt status of the failing command
 *   1 3  StorPortInitialize status at load, then the SDHCI present-state register at card init
 *   1 4  stage history, newest stage in the low byte
 *   1 5  other requests: bits 31:24 last non-SCSI SRB function, 23:16 its status, 15:0 count
 * Every Storport callback runs inside __try/__except: an exception becomes a red line at the top left
 * (1, site, code, image offset, address) and a failed request instead of a stop.
 *
 * ACPI: \_SB.SDH1, HID MRVL0007, Memory32Fixed 0xD4280000/0x1000 [+ 0xD4282854/0x8], Interrupt 71.
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

/* ---- on-screen trace ----------------------------------------------------------------------------- */
#define GT_ID      1
#define GT_FB_PHYS 0x17000000ULL
#define GT_FB_W    1024
#define GT_FB_H    600
#define GT_SCALE   3

__declspec(dllimport) PVOID MmMapIoSpace(PHYSICAL_ADDRESS PhysicalAddress, SIZE_T NumberOfBytes, ULONG CacheType);
void *__cdecl _exception_info(void);
extern UCHAR __ImageBase;

static PULONG gFb;
static LONG   gCaught;
static ULONG  gHist;
static const USHORT gGlyph[16] = {
    0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7292,
    0x7BEF, 0x7BCF, 0x2BED, 0x6BAE, 0x7927, 0x6B6E, 0x79E7, 0x79E4
};

static VOID TrRect(ULONG X, ULONG Y, ULONG W, ULONG H, ULONG Color)
{
    ULONG x, y;
    for (y = Y; y < Y + H && y < GT_FB_H; y++)
        for (x = X; x < X + W && x < GT_FB_W; x++)
            gFb[y * GT_FB_W + x] = Color;
}

static ULONG TrHex(ULONG X, ULONG Y, ULONG Value, ULONG Digits, ULONG Fg, ULONG Bg)
{
    ULONG d, r, c;
    TrRect(X, Y, Digits * 4 * GT_SCALE + GT_SCALE * 2, 5 * GT_SCALE, Bg);
    for (d = 0; d < Digits; d++) {
        USHORT g = gGlyph[(Value >> (4 * (Digits - 1 - d))) & 0xF];
        for (r = 0; r < 5; r++)
            for (c = 0; c < 3; c++)
                if (g & (1u << (14 - (r * 3 + c))))
                    TrRect(X + (d * 4 + c) * GT_SCALE, Y + r * GT_SCALE, GT_SCALE, GT_SCALE, Fg);
    }
    return X + Digits * 4 * GT_SCALE + GT_SCALE * 2;
}

/* slots 0..5: rows 7..12 (rows 9..12 belong to goyapinmux and goyagpio, which are not on the card) */
static VOID TrValue(ULONG Slot, ULONG Value)
{
    ULONG x = GT_FB_W - 12 * 4 * GT_SCALE, y = 8 + (7 + Slot) * 6 * GT_SCALE;
    if (!gFb || Slot > 5) return;
    x = TrHex(x, y, GT_ID, 1, 0x00FFFFFF, 0x00202020);
    x = TrHex(x, y, Slot, 1, 0x00808080, 0x00202020);
    TrHex(x, y, Value, 8, 0x0000FFFF, 0x00202020);
}

static VOID TrStep(ULONG Site)
{
    ULONG x = GT_FB_W - 12 * 4 * GT_SCALE, y = 8;
    if (!gFb) return;
    if ((gHist & 0xFF) != Site || gHist == 0) { gHist = (gHist << 8) | Site; TrValue(4, gHist); }
    x = TrHex(x, y, GT_ID, 1, 0x00FFFFFF, 0x00202020);
    TrHex(x, y, Site, 2, 0x0000FF00, 0x00202020);
}

typedef struct { ULONG *Record; PVOID Context; } TR_EXPTRS;   /* EXCEPTION_POINTERS */

static LONG TrFilter(PVOID Info, ULONG Site)
{
    TR_EXPTRS *e = (TR_EXPTRS *)Info;
    ULONG code = 0, addr = 0, off, base = (ULONG)(ULONG_PTR)&__ImageBase;
    if (e && e->Record) { code = e->Record[0]; addr = e->Record[3]; }   /* ExceptionCode, ExceptionAddress */
    off = (addr >= base && addr - base < 0x100000) ? addr - base : 0xFFFFFFFF;
    if (!gCaught && gFb) {
        gCaught = 1;
        ULONG x = 8, y = 8;
        x = TrHex(x, y, GT_ID, 1, 0x00FFFFFF, 0x00800000);
        x = TrHex(x, y, Site, 2, 0x00FFFFFF, 0x00800000);
        x = TrHex(x, y, code, 8, 0x00FFFF00, 0x00800000);
        x = TrHex(x, y, off, 8, 0x00FFFFFF, 0x00800000);
        TrHex(x, y, addr, 8, 0x00C0C0C0, 0x00800000);
    }
    return 1;                                       /* EXCEPTION_EXECUTE_HANDLER */
}

#define TR_GUARD(Site, Body, OnException) \
    do { __try { Body; } __except (TrFilter(_exception_info(), Site)) { OnException; } } while (0)

/* The other two SD controllers on the same interrupt line (Wi-Fi SDH2, eMMC SDH3) and their clock
   registers (Linux clk-of-pxa988: APMU_SDH1 0x58 clocks 0xD4280800, APMU_SDH2 0xE0 clocks 0xD4281000).
   They are only touched when their clock and reset bits are on. */
#define SDH_WIFI_PHYS     0xD4280800ULL
#define SDH_EMMC_PHYS     0xD4281000ULL
#define APMU_EMMC_CLK     0xD42828E0ULL
#define SDH_CLK_ON        0x0013          /* RST_N | AXI_RST_N | FCLK_EN */

/* fallback physical addresses (same as the DSDT _CRS of \_SB.SDH1) */
#define SDH1_PHYS         0xD4280000ULL
#define APMU_SDH_PHYS     0xD4282854ULL

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

static volatile UCHAR *gShared[3];     /* SDH1, SDH2, SDH3 register bases that are safe to read */

typedef struct _GOYA_EXT {
    SD_HOST  Sd;
    PUCHAR   Apmu;
    BOOLEAN  InitDone;
    UCHAR    Sense[3];                  /* key, asc, ascq of the last error */
    ULONG    InitFails;                 /* consecutive failed card initialisations */
    ULONG    Ios, Fails, Others;
    UCHAR    LastOp;
} GOYA_EXT, *PGOYA_EXT;

static VOID QuietSharedLine(PGOYA_EXT X);

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

static VOID TrError(PGOYA_EXT X, ULONG St)
{
    TrValue(2, (St << 28) | (X->Sd.LastIntSts & 0x0FFFFFFF));
}

static BOOLEAN GoyaInitCard(PGOYA_EXT X)
{
    ULONG tries, st = SD_EPARAM;
    TrStep(0x04);
    GoyaClocks(X);
    for (tries = 0; tries < 3; tries++) {
        st = SdInitCard(&X->Sd);
        if (st == SD_OK) break;
        if (X->Sd.SpeedLevel < 3) X->Sd.SpeedLevel++;
    }
    TrValue(3, SD_RD32(&X->Sd, SDHC_PRESENT));
    if (st != SD_OK) {
        X->InitFails++;
        TrError(X, st);
        TrStep(0x0F);
        return FALSE;
    }
    X->InitFails = 0;
    TrValue(0, (X->Sd.SpeedLevel << 28) | (X->Sd.Wide ? 1UL << 27 : 0) | (X->Sd.HighSpeed ? 1UL << 26 : 0) |
               (X->Sd.HighCap ? 1UL << 25 : 0) | ((ULONG)(X->Sd.Blocks >> 11) & 0x00FFFFFF));
    TrStep(0x05);
    return TRUE;
}

/* ---- Storport entry points ---- */

/* StorPortGetDeviceBase only maps an address whose resource entry has the same interface type and
   bus number as passed in (build 8520: the lookup at storport+0x1C064). The ACPI device's resources
   do not carry Cfg->AdapterInterfaceType (ACPIBus) there, so try the other likely types too. */
static PVOID DeviceBase(PGOYA_EXT X, PPORT_CONFIGURATION_INFORMATION Cfg, PACCESS_RANGE R)
{
    static const INTERFACE_TYPE types[] = { Internal, ACPIBus, PNPBus, Isa, PCIBus };
    PVOID p;
    ULONG t, b;
    p = StorPortGetDeviceBase(X, Cfg->AdapterInterfaceType, Cfg->SystemIoBusNumber,
                              R->RangeStart, R->RangeLength, (BOOLEAN)!R->RangeInMemory);
    for (t = 0; p == NULL && t < GOYA_COUNT(types); t++)
        for (b = 0; p == NULL && b < 2; b++)
            p = StorPortGetDeviceBase(X, types[t], b ? Cfg->SystemIoBusNumber : 0,
                                      R->RangeStart, R->RangeLength, (BOOLEAN)!R->RangeInMemory);
    return p;
}

static ULONG GoyaFindAdapter_(PVOID Ext, PVOID HwContext, PVOID BusInfo, PCHAR Args,
                             PPORT_CONFIGURATION_INFORMATION Cfg, PBOOLEAN Again)
{
    PGOYA_EXT X = (PGOYA_EXT)Ext;
    ULONG i;
    UNREFERENCED_PARAMETER(HwContext); UNREFERENCED_PARAMETER(BusInfo);
    UNREFERENCED_PARAMETER(Args); UNREFERENCED_PARAMETER(Again);

    TrStep(0x01);
    RtlZeroMemory(X, sizeof(*X));
    X->Sd.Os = X;
    for (i = 0; i < Cfg->NumberOfAccessRanges; i++) {
        PACCESS_RANGE r = &(*Cfg->AccessRanges)[i];
        if (r->RangeLength == 0) continue;
        if (!X->Sd.Regs && r->RangeLength >= 0x100) {
            X->Sd.Regs = (volatile UCHAR *)DeviceBase(X, Cfg, r);
        } else if (!X->Apmu && r->RangeLength >= 8) {
            X->Apmu = (PUCHAR)DeviceBase(X, Cfg, r);
        }
    }
    /* What Storport handed over (overwritten later by the card values if all goes well):
       1 0 = start of range 0, 1 1 = interrupt (see below), 1 2 = count (31:28), range 0 in memory (24), range 0 length,
       1 3 = interface type (31:16), bit 1 = registers mapped by Storport, bit 0 = APMU range mapped */
    {
        PACCESS_RANGE r0 = Cfg->NumberOfAccessRanges > 0 ? &(*Cfg->AccessRanges)[0] : NULL;
        PACCESS_RANGE r1 = Cfg->NumberOfAccessRanges > 1 ? &(*Cfg->AccessRanges)[1] : NULL;
        TrValue(0, r0 ? r0->RangeStart.LowPart : 0);
        /* 1 1 = the interrupt Storport found: mode (31:28, 0 level, 1 latched), level (27:16), vector */
        TrValue(1, ((ULONG)Cfg->InterruptMode << 28) | ((Cfg->BusInterruptLevel & 0xFFF) << 16) |
                   (Cfg->BusInterruptVector & 0xFFFF));
        UNREFERENCED_PARAMETER(r1);
        TrValue(2, (Cfg->NumberOfAccessRanges << 28) | (r0 && r0->RangeInMemory ? 1UL << 24 : 0) |
                   (r0 ? r0->RangeLength & 0x00FFFFFF : 0));
        TrValue(3, ((ULONG)Cfg->AdapterInterfaceType << 16) | (X->Sd.Regs ? 2 : 0) | (X->Apmu ? 1 : 0));
    }

    /* Build 8520's Storport did not give us a usable register range (stage 0E on hardware): map the
       controller and the APMU clock registers ourselves. Register access goes through the
       StorPortRead/WriteRegister routines either way. */
    if (!X->Sd.Regs) {
        PHYSICAL_ADDRESS pa;
        pa.QuadPart = SDH1_PHYS;
        X->Sd.Regs = (volatile UCHAR *)MmMapIoSpace(pa, 0x1000, 0 /* MmNonCached */);
        if (X->Sd.Regs) TrStep(0x0B);
    } else {
        TrStep(0x0A);
    }
    if (!X->Apmu) {
        PHYSICAL_ADDRESS pa;
        pa.QuadPart = APMU_SDH_PHYS;
        X->Apmu = (PUCHAR)MmMapIoSpace(pa, 8, 0 /* MmNonCached */);
    }
    if (!X->Sd.Regs) { TrStep(0x0E); return SP_RETURN_BAD_CONFIG; }

    /* make sure a stale firmware setting can never raise an interrupt */
    SD_WR32(&X->Sd, SDHC_SIGEN, 0);
    SD_WR32(&X->Sd, SDHC_INTSTS, 0xFFFFFFFF);
    QuietSharedLine(X);

    Cfg->MaximumTransferLength = MAX_XFER_BLOCKS * SD_BLOCK;
    Cfg->NumberOfPhysicalBreaks = MAX_XFER_BLOCKS + 1;
    Cfg->AlignmentMask = 0;
    Cfg->NumberOfBuses = 1;
    Cfg->MaximumNumberOfTargets = 1;
    Cfg->MaximumNumberOfLogicalUnits = 1;
    /* Build 8520's Storport fails the start with STATUS_DEVICE_CONFIGURATION_ERROR (C0000182) unless
       NeedPhysicalAddresses, TaggedQueuing, ScatterGather and Master are all TRUE (storport+0x1EDA0),
       even for a polled PIO miniport. Requests are still completed one by one in StartIo. */
    Cfg->ScatterGather = TRUE;
    Cfg->Master = TRUE;
    Cfg->NeedPhysicalAddresses = TRUE;
    Cfg->TaggedQueuing = TRUE;
    Cfg->MultipleRequestPerLu = TRUE;
    /* No DMA: with VirtualDevice FALSE, build 8520's Storport connects the interrupt, sets up DMA and
       calls KeFlushIoBuffers(Mdl, Read, DmaOperation=TRUE) on every data buffer (storport+0x5CB8),
       and the ARM kernel stops with DRIVER_VERIFIER_DMA_VIOLATION 0xE6/0x1F when a read buffer is
       not cache-line aligned (classpnp's small buffers are not). VirtualDevice TRUE (adapter+0x16C
       in Storport) skips the interrupt, the DMA adapter and the flushes; data is copied by the CPU
       through StorPortGetSystemAddress. */
    Cfg->VirtualDevice = TRUE;
    Cfg->CachesData = FALSE;
    Cfg->MapBuffers = STOR_MAP_ALL_BUFFERS_INCLUDING_READ_WRITE;
    Cfg->InitiatorBusId[0] = 7;
    TrStep(0x02);
    return SP_RETURN_FOUND;
}

static BOOLEAN GoyaPassiveInit_(PVOID Ext)
{
    PGOYA_EXT X = (PGOYA_EXT)Ext;
    X->InitDone = TRUE;
    if (!GoyaInitCard(X)) StorPortDebugPrint(0, "goyasdhc: card initialisation failed\n");
    return TRUE;                        /* keep the adapter; I/O will retry the card init */
}

/* The card is initialised right here (DISPATCH_LEVEL, no interrupt connected; usually well under a
   second). Build 8520's Storport accepted StorPortEnablePassiveInitialization but never called the
   routine, and the adapter then never enumerated its disk. */
static BOOLEAN GoyaInitialize_(PVOID Ext)
{
    TrStep(0x03);
    return GoyaPassiveInit_(Ext);
}

/* Polled driver: nothing should ever signal. If a controller on the shared line does (its signal enable
   was set by someone), switch its signal enable off and claim the interrupt, so the level line drops. */
static BOOLEAN GoyaInterrupt(PVOID Ext)
{
    BOOLEAN mine = FALSE;
    ULONG i;
    for (i = 0; i < 3; i++) {
        volatile UCHAR *r = gShared[i];
        if (r && (StorPortReadRegisterUlong(Ext, (PULONG)(r + SDHC_SIGEN)) &
                  StorPortReadRegisterUlong(Ext, (PULONG)(r + SDHC_INTSTS)))) {
            StorPortWriteRegisterUlong(Ext, (PULONG)(r + SDHC_SIGEN), 0);
            mine = TRUE;
        }
    }
    return mine;
}

/* Signal enables of the other SD controllers on the interrupt line to 0 (only if they are clocked). */
static VOID QuietSharedLine(PGOYA_EXT X)
{
    PHYSICAL_ADDRESS pa;
    PULONG clk;
    gShared[0] = X->Sd.Regs;
    if (X->Apmu && (StorPortReadRegisterUlong(X, (PULONG)(X->Apmu + APMU_SDH1_OFF)) & SDH_CLK_ON) == SDH_CLK_ON &&
        (StorPortReadRegisterUlong(X, (PULONG)(X->Apmu + APMU_SDH0_OFF)) & SDH_CLK_ACLK_EN)) {
        pa.QuadPart = SDH_WIFI_PHYS;
        gShared[1] = (volatile UCHAR *)MmMapIoSpace(pa, 0x100, 0);
    }
    pa.QuadPart = APMU_EMMC_CLK;
    clk = (PULONG)MmMapIoSpace(pa, 4, 0);
    if (clk && (StorPortReadRegisterUlong(X, clk) & SDH_CLK_ON) == SDH_CLK_ON && X->Apmu &&
        (StorPortReadRegisterUlong(X, (PULONG)(X->Apmu + APMU_SDH0_OFF)) & SDH_CLK_ACLK_EN)) {
        pa.QuadPart = SDH_EMMC_PHYS;
        gShared[2] = (volatile UCHAR *)MmMapIoSpace(pa, 0x100, 0);
    }
    if (gShared[1]) StorPortWriteRegisterUlong(X, (PULONG)(gShared[1] + SDHC_SIGEN), 0);
    if (gShared[2]) StorPortWriteRegisterUlong(X, (PULONG)(gShared[2] + SDHC_SIGEN), 0);
}

static BOOLEAN GoyaResetBus_(PVOID Ext, ULONG PathId)
{
    PGOYA_EXT X = (PGOYA_EXT)Ext;
    X->Sd.Ready = FALSE;
    X->InitFails = 0;
    GoyaInitCard(X);
    StorPortCompleteRequest(Ext, (UCHAR)PathId, SP_UNTAGGED, SP_UNTAGGED, SRB_STATUS_BUS_RESET);
    return TRUE;
}

static SCSI_ADAPTER_CONTROL_STATUS GoyaAdapterControl_(PVOID Ext, SCSI_ADAPTER_CONTROL_TYPE Type, PVOID Param)
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
        X->InitFails = 0;
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

/* System address of the request's data buffer. Srb->DataBuffer is only a mapped system address for
   READ/WRITE when Storport honours STOR_MAP_ALL_BUFFERS_INCLUDING_READ_WRITE (Windows 8 final); the
   8520 build may hand over the caller's original address instead, which is not valid in the thread
   StartIo runs in. StorPortGetSystemAddress maps the request's MDL in every Storport version. */
static PUCHAR SrbData(PGOYA_EXT X, PSCSI_REQUEST_BLOCK Srb)
{
    PVOID va = NULL;
    if (!Srb->DataBuffer || !Srb->DataTransferLength) return NULL;
    if (StorPortGetSystemAddress(X, Srb, &va) == STOR_STATUS_SUCCESS && va) return (PUCHAR)va;
    return NULL;
}

static VOID ReturnData(PGOYA_EXT X, PSCSI_REQUEST_BLOCK Srb, const VOID *Data, ULONG Len)
{
    PUCHAR buf;
    if (Len > Srb->DataTransferLength) Len = Srb->DataTransferLength;
    buf = Len ? SrbData(X, Srb) : NULL;
    if (Len && !buf) { Srb->DataTransferLength = 0; Srb->SrbStatus = SRB_STATUS_ERROR; return; }
    if (Len) RtlCopyMemory(buf, Data, Len);
    Srb->DataTransferLength = Len;
    Srb->SrbStatus = SRB_STATUS_SUCCESS;
}

static ULONG Be32(const UCHAR *p) { return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | p[3]; }
static VOID  PutBe32(PUCHAR p, ULONG v) { p[0] = (UCHAR)(v >> 24); p[1] = (UCHAR)(v >> 16); p[2] = (UCHAR)(v >> 8); p[3] = (UCHAR)v; }

/* StartIo runs at DISPATCH_LEVEL: after 3 failed initialisations in a row the card is reported as not
   ready without trying again, until a bus/device reset or a restart clears the count. */
static BOOLEAN EnsureCard(PGOYA_EXT X)
{
    if (!X->Sd.Regs) return FALSE;
    if (!X->Sd.Ready && X->InitFails < 3) GoyaInitCard(X);
    return X->Sd.Ready;
}

static VOID DoReadWrite(PGOYA_EXT X, PSCSI_REQUEST_BLOCK Srb, BOOLEAN Write)
{
    const UCHAR *cdb = Srb->Cdb;
    ULONGLONG lba;
    ULONG n, done;
    SD_STATUS st;
    PUCHAR data = NULL;

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
    if ((ULONGLONG)n * SD_BLOCK > Srb->DataTransferLength || (data = SrbData(X, Srb)) == NULL) {
        SetSense(Srb, X, KEY_ILLEGAL_REQUEST, 0x24, 0); return;
    }

    done = 0;
    while (n) {
        ULONG chunk = n > MAX_XFER_BLOCKS ? MAX_XFER_BLOCKS : n;
        PUCHAR p = data + done * SD_BLOCK;
        st = SdTransfer(&X->Sd, (BOOLEAN)!Write, lba, chunk, p);
        done += chunk;
        if (st != SD_OK) {
            X->Sd.LastIntSts = X->Sd.FailIntSts;
            TrError(X, st);
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
            ReturnData(X, Srb, d, 6); return;
        case 0x80: {                             /* unit serial = CID product serial number */
            ULONG psn = SdCsdBits(X->Sd.Cid, 24, 32), i;
            d[1] = 0x80; d[3] = 8;
            for (i = 0; i < 8; i++) d[4 + i] = "0123456789ABCDEF"[(psn >> (28 - 4 * i)) & 0xF];
            ReturnData(X, Srb, d, 12); return;
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
    ReturnData(X, Srb, d, 36);
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
    ReturnData(X, Srb, d, len);
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
        ReturnData(X, Srb, d, 18);
        break;
    case OP_INQUIRY:
        DoInquiry(X, Srb);
        break;
    case OP_READ_CAPACITY:
        if (!EnsureCard(X)) { SetSense(Srb, X, KEY_NOT_READY, 0x3A, 0); break; }
        PutBe32(d, X->Sd.Blocks - 1 > 0xFFFFFFFFULL ? 0xFFFFFFFFUL : (ULONG)(X->Sd.Blocks - 1));
        PutBe32(d + 4, SD_BLOCK);
        ReturnData(X, Srb, d, 8);
        break;
    case OP_SAI16:
        if ((Srb->Cdb[1] & 0x1F) != 0x10) { SetSense(Srb, X, KEY_ILLEGAL_REQUEST, 0x20, 0); break; }
        if (!EnsureCard(X)) { SetSense(Srb, X, KEY_NOT_READY, 0x3A, 0); break; }
        PutBe32(d, (ULONG)((X->Sd.Blocks - 1) >> 32));
        PutBe32(d + 4, (ULONG)(X->Sd.Blocks - 1));
        PutBe32(d + 8, SD_BLOCK);
        ReturnData(X, Srb, d, 32);
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
        ReturnData(X, Srb, d, 16);
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

static VOID GoyaStartIo_(PVOID Ext, PSCSI_REQUEST_BLOCK Srb)
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
        X->InitFails = 0;
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
}

/* ---- guarded Storport callbacks ---- */

static ULONG GoyaFindAdapter(PVOID Ext, PVOID HwContext, PVOID BusInfo, PCHAR Args,
                             PPORT_CONFIGURATION_INFORMATION Cfg, PBOOLEAN Again)
{
    ULONG r = SP_RETURN_ERROR;
    TR_GUARD(0x01, r = GoyaFindAdapter_(Ext, HwContext, BusInfo, Args, Cfg, Again), r = SP_RETURN_ERROR);
    return r;
}

static BOOLEAN GoyaInitialize(PVOID Ext)
{
    BOOLEAN r = TRUE;
    TR_GUARD(0x03, r = GoyaInitialize_(Ext), r = TRUE);
    return r;
}

static BOOLEAN GoyaResetBus(PVOID Ext, ULONG PathId)
{
    BOOLEAN r = TRUE;
    TR_GUARD(0x07, r = GoyaResetBus_(Ext, PathId), r = TRUE);
    return r;
}

static SCSI_ADAPTER_CONTROL_STATUS GoyaAdapterControl(PVOID Ext, SCSI_ADAPTER_CONTROL_TYPE Type, PVOID Param)
{
    SCSI_ADAPTER_CONTROL_STATUS r = ScsiAdapterControlUnsuccessful;
    TR_GUARD(0x08, r = GoyaAdapterControl_(Ext, Type, Param), r = ScsiAdapterControlUnsuccessful);
    return r;
}

static BOOLEAN GoyaStartIo(PVOID Ext, PSCSI_REQUEST_BLOCK Srb)
{
    PGOYA_EXT X = (PGOYA_EXT)Ext;
    if (X->Ios == 0 && X->Others == 0) TrStep(0x06);
    TR_GUARD(0x06, GoyaStartIo_(Ext, Srb), Srb->SrbStatus = SRB_STATUS_ERROR);
    if (Srb->Function == SRB_FUNCTION_EXECUTE_SCSI) {
        X->Ios++;
        X->LastOp = Srb->Cdb[0];
        if (SRB_STATUS(Srb->SrbStatus) != SRB_STATUS_SUCCESS) X->Fails++;
        TrValue(1, ((ULONG)X->LastOp << 24) | ((X->Fails & 0xFF) << 16) | (X->Ios & 0xFFFF));
    } else {
        X->Others++;
        TrValue(5, ((ULONG)Srb->Function << 24) | ((ULONG)Srb->SrbStatus << 16) | (X->Others & 0xFFFF));
    }
    StorPortNotification(RequestComplete, Ext, Srb);
    return TRUE;
}

ULONG DriverEntry(PVOID DriverObject, PVOID RegistryPath)
{
    static const INTERFACE_TYPE types[] = { Internal, ACPIBus, PNPBus, Isa };
    HW_INITIALIZATION_DATA h;
    ULONG i, st, best = 0xC0000001UL;   /* STATUS_UNSUCCESSFUL */
    PHYSICAL_ADDRESS fb;

    fb.QuadPart = GT_FB_PHYS;
    gFb = (PULONG)MmMapIoSpace(fb, GT_FB_W * GT_FB_H * 4, 0 /* MmNonCached */);
    TrStep(0x00);

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
        h.TaggedQueuing = TRUE;                 /* required by Storport, see GoyaFindAdapter_ */
        h.AutoRequestSense = TRUE;
        h.MultipleRequestPerLu = TRUE;
        st = StorPortInitialize(DriverObject, RegistryPath, &h, NULL);
        if (st == 0 || best != 0) best = st;
    }
    TrValue(3, best);
    return best;
}
