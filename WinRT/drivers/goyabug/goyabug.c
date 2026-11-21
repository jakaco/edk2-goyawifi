/* goyabug - writes the stop code, its parameters and the driver they point into on the stop screen.

   The Windows 8 build 8520 stop screen shows only the name of the stop (no parameters) and WinPE keeps
   no crash dump, so an intermittent KMODE_EXCEPTION_NOT_HANDLED cannot be traced. This boot-start driver
   uses only documented interfaces: KeRegisterBugCheckCallback (the routine runs while the stop screen
   is up) and the exported KiBugCheckData (the stop code and its four parameters). Nothing in the kernel
   is patched or hooked.

   At a stop it draws a black box at the bottom of the screen:
     STOP <code>  P1 .. P4                      the parameters
     P<n> <driver>+<offset>                     for each parameter that is an address inside a driver
     PC / LR <driver>+<offset>                  for stops that pass a CONTEXT (0x7E, 0x1000007E)
     stack: <driver>+<offset> ...               Thumb return addresses found on the current stack
                                                (heuristic call chain, newest first)
   Drivers are looked up in a copy of the loaded-module list that a thread refreshes every 2 s (the
   list itself cannot be walked safely at stop time).

   Non-PnP kernel service, Start 0 (boot), no INF: make-goya-sd.cmd / install-winrt-sd.cmd copy
   goyabug.sys to System32\drivers and create the service key. ErrorControl 0: if it cannot load,
   Windows boots without it.
*/
#define GOYA_TRACE_ID 7
#include <ntddk.h>
#include "..\common\goyatrace.h"
#include "..\common\gtfont.h"

#define SystemModuleInformation 11
NTSYSAPI NTSTATUS NTAPI ZwQuerySystemInformation(ULONG Class, PVOID Buffer, ULONG Length, PULONG ReturnLength);

typedef struct _MODINFO {
    HANDLE Section;
    PVOID  MappedBase;
    PVOID  ImageBase;
    ULONG  ImageSize;
    ULONG  Flags;
    USHORT LoadOrderIndex, InitOrderIndex, LoadCount, OffsetToFileName;
    UCHAR  FullPathName[256];
} MODINFO;

typedef struct _MODLIST {
    ULONG   Count;
    MODINFO M[1];
} MODLIST;

#define LIST_BYTES (128 * 1024)

static PUCHAR gList[2];
static volatile LONG gActive = -1;           /* index of the valid snapshot, -1 = none */
static PULONG_PTR gBugData;                  /* KiBugCheckData[5] */
static KBUGCHECK_CALLBACK_RECORD gRecord;

/* ---- module snapshot (PASSIVE_LEVEL) --------------------------------------------------------- */

static VOID Snapshot(VOID)
{
    LONG next = (gActive == 0) ? 1 : 0;
    ULONG ret = 0;
    if (gList[next] == NULL) return;
    if (NT_SUCCESS(ZwQuerySystemInformation(SystemModuleInformation, gList[next], LIST_BYTES, &ret))) {
        InterlockedExchange(&gActive, next);
    }
}

static VOID SnapshotThread(PVOID Context)
{
    LARGE_INTEGER t;
    UNREFERENCED_PARAMETER(Context);
    t.QuadPart = -20000000LL;                /* 2 s */
    for (;;) {
        KeDelayExecutionThread(KernelMode, FALSE, &t);
        Snapshot();
    }
}

/* ---- text output at stop time (no allocation, no locks) ---------------------------------------- */

#define TS 2                                 /* font scale: 14 x 24 pixel cells */
#define CW (7 * TS)
#define CH (12 * TS)
#define COLS (GT_FB_W / CW)
#define BOX_LINES 11
#define BOX_Y (GT_FB_H - BOX_LINES * CH)

static ULONG gCol, gRow;

static VOID PutCh(PULONG fb, char c, ULONG fg)
{
    const UCHAR *g;
    ULONG x0, y0, x, y;
    if (c == '\n' || gCol >= COLS) { gCol = 0; gRow++; if (c == '\n') return; }
    if (gRow >= BOX_LINES) return;
    if (c < 32 || c > 126) c = '?';
    g = gGtFont[c - 32];
    x0 = gCol * CW; y0 = BOX_Y + gRow * CH;
    for (y = 0; y < CH; y++)
        for (x = 0; x < CW; x++)
            fb[(y0 + y) * GT_FB_W + x0 + x] = ((g[y / TS] >> (6 - x / TS)) & 1) ? fg : 0;
    gCol++;
}

static VOID PutS(PULONG fb, const char *s, ULONG fg) { while (*s) PutCh(fb, *s++, fg); }

static VOID PutHex(PULONG fb, ULONG v, ULONG digits, ULONG fg)
{
    while (digits--) PutCh(fb, "0123456789ABCDEF"[(v >> (digits * 4)) & 15], fg);
}

/* Find the module containing Addr in the snapshot. Returns the file name (or NULL) and the offset. */
static const char *Lookup(ULONG Addr, PULONG Off)
{
    LONG a = gActive;
    MODLIST *l;
    ULONG i;
    if (a < 0 || Addr < 0x80000000UL) return NULL;
    l = (MODLIST *)gList[a];
    for (i = 0; i < l->Count && (PUCHAR)&l->M[i + 1] <= gList[a] + LIST_BYTES; i++) {
        ULONG b = (ULONG)(ULONG_PTR)l->M[i].ImageBase;
        if (Addr >= b && Addr - b < l->M[i].ImageSize) {
            USHORT o = l->M[i].OffsetToFileName;
            *Off = Addr - b;
            return (const char *)l->M[i].FullPathName + (o < 255 ? o : 0);
        }
    }
    return NULL;
}

/* "name+offset" (name cut to 14 characters); returns FALSE if Addr is in no known module */
static BOOLEAN PutAddr(PULONG fb, ULONG Addr, ULONG fg)
{
    ULONG off = 0, n = 0;
    const char *name = Lookup(Addr, &off);
    if (name == NULL) return FALSE;
    while (name[n] && n < 14) PutCh(fb, name[n++], fg);
    PutCh(fb, '+', fg);
    PutHex(fb, off, (off >> 16) ? 6 : 4, fg);
    return TRUE;
}

static VOID BugCheckCallback(PVOID Buffer, ULONG Length)
{
    PULONG fb = gGtFb;
    ULONG code, p[4], i, hits = 0;
    ULONG_PTR sp, end;
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(Length);

    if (fb == NULL || gBugData == NULL) return;
    code = (ULONG)gBugData[0];
    for (i = 0; i < 4; i++) p[i] = (ULONG)gBugData[i + 1];

    GtRect(fb, 0, BOX_Y, GT_FB_W, GT_FB_H - BOX_Y, 0);
    gCol = 0; gRow = 0;
    PutS(fb, "STOP ", 0x00FFFFFF); PutHex(fb, code, 8, 0x00FFFF00);
    for (i = 0; i < 4; i++) {
        PutS(fb, "  P", 0x00FFFFFF); PutCh(fb, (char)('1' + i), 0x00FFFFFF); PutCh(fb, ' ', 0);
        PutHex(fb, p[i], 8, 0x0000FFFF);
    }
    PutCh(fb, '\n', 0);
    if (gActive < 0) PutS(fb, "(no module list yet)\n", 0x00808080);

    for (i = 0; i < 4; i++) {
        ULONG c0 = gCol;
        PutS(fb, "P", 0x00FFFFFF); PutCh(fb, (char)('1' + i), 0x00FFFFFF); PutCh(fb, ' ', 0);
        if (PutAddr(fb, p[i], 0x0000FF00)) { PutS(fb, "   ", 0); hits++; }
        else gCol = c0;                                         /* not an address: take it back */
    }
    if (hits) PutCh(fb, '\n', 0);

    /* SYSTEM_THREAD_EXCEPTION_NOT_HANDLED: P3 = exception record, P4 = context record */
    if ((code == 0x7E || code == 0x1000007E) && p[3] >= 0x80000000UL && MmIsAddressValid((PVOID)(ULONG_PTR)p[3])) {
        PCONTEXT ctx = (PCONTEXT)(ULONG_PTR)p[3];
        PutS(fb, "PC ", 0x00FFFFFF);
        if (!PutAddr(fb, ctx->Pc, 0x0000FF00)) PutHex(fb, ctx->Pc, 8, 0x0000FFFF);
        PutS(fb, "  LR ", 0x00FFFFFF);
        if (!PutAddr(fb, ctx->Lr & ~1UL, 0x0000FF00)) PutHex(fb, ctx->Lr, 8, 0x0000FFFF);
        PutCh(fb, '\n', 0);
    }

    /* Heuristic call chain: odd words (Thumb return addresses) on this stack that lie in a module.
       Pages are checked before they are read, so the scan stops at the end of the stack. */
    PutS(fb, "stack:", 0x00FFFFFF);
    sp = (ULONG_PTR)&code & ~3UL;
    end = sp + 16 * 1024;
    hits = 0;
    for (; sp < end && hits < 21 && gRow < BOX_LINES; sp += 4) {
        ULONG v, off;
        if ((sp & (PAGE_SIZE - 1)) == 0 || sp == ((ULONG_PTR)&code & ~3UL)) {
            if (!MmIsAddressValid((PVOID)sp)) break;
        }
        v = *(volatile ULONG *)sp;
        if (!(v & 1) || Lookup(v & ~1UL, &off) == NULL) continue;
        if (gCol + 24 > COLS) PutCh(fb, '\n', 0);
        PutCh(fb, ' ', 0);
        PutAddr(fb, (v & ~1UL) - 2, (hits & 1) ? 0x00C0C0C0 : 0x00FFFFFF);   /* -2: the call, not the return */
        hits++;
    }
}

/* ---- entry ------------------------------------------------------------------------------------- */

NTSTATUS DriverEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING n;
    HANDLE h;
    UNREFERENCED_PARAMETER(RegistryPath);
    UNREFERENCED_PARAMETER(Driver);                  /* no DriverUnload: stays loaded */

    GtFb();                                          /* map the framebuffer now: not possible at stop time */
    RtlInitUnicodeString(&n, L"KiBugCheckData");
    gBugData = (PULONG_PTR)MmGetSystemRoutineAddress(&n);
    gList[0] = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, LIST_BYTES, 'gubG');
    gList[1] = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, LIST_BYTES, 'gubG');
    Snapshot();

    KeInitializeCallbackRecord(&gRecord);
    if (!KeRegisterBugCheckCallback(&gRecord, BugCheckCallback, NULL, 0, (PUCHAR)"goyabug")) {
        GoyaTraceStep(0xEE);
        return STATUS_UNSUCCESSFUL;
    }
    if (NT_SUCCESS(PsCreateSystemThread(&h, THREAD_ALL_ACCESS, NULL, NULL, NULL, SnapshotThread, NULL))) ZwClose(h);
    GoyaTraceStep(gBugData != NULL && gActive >= 0 ? 0x01 : 0x0E);
    return STATUS_SUCCESS;
}
