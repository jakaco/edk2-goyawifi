/*
 * sdcore.c - self-contained SD memory card protocol + SDHCI (PXAv3) host code, polled PIO.
 *
 * Used by goyasdhc.c (Windows storport miniport) and by tools/sdsim (Linux simulator test).
 * The includer defines, before including this file:
 *   SD_RD8/16/32(c, off), SD_WR8/16/32(c, off, v)   register access on SD_HOST *c
 *   SD_STALL(us)                                     busy wait
 *   SD_LOG(fmt, ...)                                 debug print (may be empty)
 *   SD_DATA_PORT(c)  (optional) volatile ULONG * to the buffer data port, for the fast block copy
 *   ULONG/USHORT/UCHAR/BOOLEAN/ULONGLONG types, TRUE/FALSE
 *
 * Nothing here depends on an OS SD stack: Windows 8.0 RT has no sdport.sys, and the inbox
 * sdbus.sys does not know the board quirks (broken base-clock capability, GPIO card detect).
 */

/* ---- SDHCI registers ---- */
#define SDHC_BLKSIZE    0x04
#define SDHC_BLKCOUNT   0x06
#define SDHC_ARG        0x08
#define SDHC_XFER       0x0C
#define SDHC_CMD        0x0E
#define SDHC_RESP       0x10
#define SDHC_BUFFER     0x20
#define SDHC_PRESENT    0x24
#define SDHC_HOSTCTRL   0x28
#define SDHC_POWER      0x29
#define SDHC_CLKCTRL    0x2C
#define SDHC_TIMEOUT    0x2E
#define SDHC_SWRESET    0x2F
#define SDHC_INTSTS     0x30
#define SDHC_INTEN      0x34
#define SDHC_SIGEN      0x38
#define SDHC_CAPS       0x40
#define SDHC_VERSION    0xFE

#define PRES_CMD_INHIBIT  0x00000001
#define PRES_DAT_INHIBIT  0x00000002
#define PRES_CARD_IN      0x00010000
#define PRES_CARD_STABLE  0x00020000
#define PRES_DAT0         0x00100000

#define HC_4BIT           0x02
#define HC_HISPEED        0x04
#define HC_8BIT           0x20
#define HC_CD_TEST_LVL    0x40
#define HC_CD_SIG_SEL     0x80

#define INT_CC            0x00000001
#define INT_TC            0x00000002
#define INT_BWR           0x00000010
#define INT_BRR           0x00000020
#define INT_ERRSUM        0x00008000
#define INT_ERRMASK       0xFFFF0000
#define INT_DTO           0x00100000
#define INT_CTO           0x00010000
#define INT_ENABLE_ALL    0x03FF0033      /* CC TC BWR BRR + all errors; never card int */

#define XFER_BLKCNT_EN    0x0002
#define XFER_ACMD12       0x0004
#define XFER_READ         0x0010
#define XFER_MULTI        0x0020

#define CMD_RSP_NONE      0x0000
#define CMD_RSP_136       0x0001
#define CMD_RSP_48        0x0002
#define CMD_RSP_48B       0x0003
#define CMD_CRC           0x0008
#define CMD_IDX           0x0010
#define CMD_DATA          0x0020
#define CMD_ABORT         0x00C0

#define R_NONE  CMD_RSP_NONE
#define R_R1    (CMD_RSP_48 | CMD_CRC | CMD_IDX)
#define R_R1B   (CMD_RSP_48B | CMD_CRC | CMD_IDX)
#define R_R2    (CMD_RSP_136 | CMD_CRC)
#define R_R3    CMD_RSP_48
#define R_R6    R_R1
#define R_R7    R_R1

#define SD_BLOCK          512
#define SD_DEFAULT_BASE   52000000UL      /* APMU SDH1: pll1_416 / 8, set by the firmware */

/* card status (R1) */
#define R1_ERRORS         0xFDF90008UL    /* out of range, address, block len, ... */
#define R1_STATE(r)       (((r) >> 9) & 0xF)
#define R1_STATE_TRAN     4
#define R1_READY          0x00000100UL

typedef struct _SD_HOST {
    volatile UCHAR *Regs;
    PVOID     Os;                 /* OS context for the register macros */
    ULONG     BaseHz;
    ULONG     SpecVer;            /* 0 = 1.0, 1 = 2.0, 2 = 3.0 */
    ULONG     ClockHz;
    ULONG     SpeedLevel;         /* 0 = HS 26 MHz, 1 = 13 MHz, 2 = 6.5 MHz, 3 = 1-bit 6.5 MHz */
    BOOLEAN   Ready;
    BOOLEAN   HighCap;            /* block addressed (SDHC/SDXC) */
    BOOLEAN   Wide;
    BOOLEAN   HighSpeed;
    ULONG     Rca;
    ULONG     Cid[4];
    ULONG     Csd[4];
    ULONGLONG Blocks;
    ULONG     LastIntSts;
    ULONG     FailIntSts;
    ULONG     ErrorCount;
    /* asynchronous transfer (SdXferBegin / SdXferPoll) */
    PUCHAR    XBuf;
    ULONG     XCount, XDone;
    BOOLEAN   XRead;
    UCHAR     XPhase;             /* 0 idle, 1 data blocks, 2 waiting for transfer complete (write busy) */
} SD_HOST;

typedef enum { SD_OK = 0, SD_ETIMEOUT, SD_ECMD, SD_EDATA, SD_ECARD, SD_ENOCARD, SD_EPARAM, SD_PENDING } SD_STATUS;

static ULONG SdCsdBits(const ULONG *r, ULONG lsb, ULONG len)
{
    /* SDHCI strips the CRC byte: response bit k = CSD/CID bit k + 8 */
    ULONG v = 0, i;
    for (i = 0; i < len; i++) {
        ULONG b = lsb + i - 8;
        v |= ((r[b / 32] >> (b % 32)) & 1) << i;
    }
    return v;
}

static BOOLEAN SdWaitClear8(SD_HOST *c, ULONG off, UCHAR mask, ULONG us)
{
    while (us) {
        if (!(SD_RD8(c, off) & mask)) return TRUE;
        SD_STALL(10); us = us > 10 ? us - 10 : 0;
    }
    return !(SD_RD8(c, off) & mask);
}

static BOOLEAN SdReset(SD_HOST *c, UCHAR mask)
{
    SD_WR8(c, SDHC_SWRESET, mask);
    return SdWaitClear8(c, SDHC_SWRESET, mask, 100000);
}

/* Host Control 1 gets cleared by a full reset: re-assert the card-detect override (the microSD
   detect line is a GPIO, not the controller CD pin) and the bus width / speed bits. */
static VOID SdHostCtrl(SD_HOST *c)
{
    UCHAR h = HC_CD_SIG_SEL | HC_CD_TEST_LVL;
    if (c->Wide) h |= HC_4BIT;
    if (c->HighSpeed) h |= HC_HISPEED;
    SD_WR8(c, SDHC_HOSTCTRL, h);
}

static SD_STATUS SdSetClock(SD_HOST *c, ULONG hz)
{
    ULONG div, us;
    USHORT clk;

    SD_WR16(c, SDHC_CLKCTRL, 0);
    if (c->SpecVer >= 2) {                         /* 10-bit divided clock: base / (2N) */
        div = (hz >= c->BaseHz) ? 0 : (c->BaseHz + 2 * hz - 1) / (2 * hz);
        if (div > 0x3FF) div = 0x3FF;
        c->ClockHz = div ? c->BaseHz / (2 * div) : c->BaseHz;
        clk = (USHORT)(((div & 0xFF) << 8) | (((div >> 8) & 3) << 6));
    } else {                                       /* 8-bit power-of-two divider */
        ULONG d = 1;
        while (d < 256 && c->BaseHz / d > hz) d <<= 1;
        c->ClockHz = c->BaseHz / d;
        clk = (USHORT)((d >> 1) << 8);
    }
    SD_WR16(c, SDHC_CLKCTRL, (USHORT)(clk | 0x0001));
    for (us = 0; us < 20000 && !(SD_RD16(c, SDHC_CLKCTRL) & 0x0002); us += 10) SD_STALL(10);
    if (!(SD_RD16(c, SDHC_CLKCTRL) & 0x0002)) return SD_ETIMEOUT;
    SD_WR16(c, SDHC_CLKCTRL, (USHORT)(clk | 0x0005));
    SD_STALL(1000);                                /* >= 74 clocks before the next command */
    return SD_OK;
}

static ULONG SdWaitInt(SD_HOST *c, ULONG mask, ULONG us)
{
    ULONG s;
    for (;;) {
        s = SD_RD32(c, SDHC_INTSTS);
        if (s & (mask | INT_ERRSUM | INT_ERRMASK)) return s;
        if (us == 0) return 0;
        SD_STALL(10); us = us > 10 ? us - 10 : 0;
    }
}

static VOID SdRecover(SD_HOST *c)
{
    SD_WR32(c, SDHC_INTSTS, 0xFFFFFFFF);
    SdReset(c, 0x02);
    SdReset(c, 0x04);
}

/* Issue one command; for data commands only the command phase is completed here. */
static SD_STATUS SdCmd(SD_HOST *c, ULONG idx, ULONG arg, USHORT flags, USHORT xfer, ULONG *resp)
{
    ULONG inhibit = PRES_CMD_INHIBIT, us, s;

    if ((flags & CMD_DATA) || (flags & 3) == CMD_RSP_48B) inhibit |= PRES_DAT_INHIBIT;
    if ((flags & CMD_ABORT) == CMD_ABORT) inhibit = PRES_CMD_INHIBIT;
    for (us = 0; us < 200000 && (SD_RD32(c, SDHC_PRESENT) & inhibit); us += 10) SD_STALL(10);
    if (SD_RD32(c, SDHC_PRESENT) & inhibit) { SdRecover(c); return SD_ETIMEOUT; }

    SD_WR32(c, SDHC_INTSTS, 0xFFFFFFFF);
    SD_WR32(c, SDHC_ARG, arg);
    SD_WR16(c, SDHC_XFER, xfer);
    SD_WR16(c, SDHC_CMD, (USHORT)((idx << 8) | flags));

    s = SdWaitInt(c, INT_CC, 100000);
    c->LastIntSts = s;
    if (!(s & INT_CC) || (s & INT_ERRMASK)) {
        SdRecover(c);
        return (s & INT_CTO) || !s ? SD_ETIMEOUT : SD_ECMD;
    }
    SD_WR32(c, SDHC_INTSTS, INT_CC);

    if (resp) {
        resp[0] = SD_RD32(c, SDHC_RESP);
        if ((flags & 3) == CMD_RSP_136) {
            resp[1] = SD_RD32(c, SDHC_RESP + 4);
            resp[2] = SD_RD32(c, SDHC_RESP + 8);
            resp[3] = SD_RD32(c, SDHC_RESP + 12);
        }
    }
    if (!(flags & CMD_DATA) && (flags & 3) == CMD_RSP_48B) {
        s = SdWaitInt(c, INT_TC, 1000000);          /* TC = end of busy */
        if (!(s & INT_TC) || (s & INT_ERRMASK)) {
            for (us = 0; us < 1000000 && !(SD_RD32(c, SDHC_PRESENT) & PRES_DAT0); us += 10) SD_STALL(10);
            SdRecover(c);
            if (!(SD_RD32(c, SDHC_PRESENT) & PRES_DAT0)) return SD_ETIMEOUT;
        }
        SD_WR32(c, SDHC_INTSTS, INT_TC);
    }
    return SD_OK;
}

static SD_STATUS SdAppCmd(SD_HOST *c, ULONG idx, ULONG arg, USHORT flags, ULONG *resp)
{
    ULONG r;
    SD_STATUS st = SdCmd(c, 55, c->Rca << 16, R_R1, 0, &r);
    if (st != SD_OK) return st;
    if (!(r & 0x20)) return SD_ECARD;                /* APP_CMD not accepted */
    return SdCmd(c, idx, arg, flags, 0, resp);
}

static SD_STATUS SdWaitTran(SD_HOST *c)
{
    ULONG r, i;
    for (i = 0; i < 1000; i++) {
        if (SdCmd(c, 13, c->Rca << 16, R_R1, 0, &r) == SD_OK &&
            (r & R1_READY) && R1_STATE(r) == R1_STATE_TRAN) return SD_OK;
        SD_STALL(1000);
    }
    return SD_ETIMEOUT;
}

/* One block through the buffer data port. With SD_DATA_PORT the port is read/written directly (one
   load/store per word instead of a call per word); word stores when the buffer is aligned. */
static VOID SdFifoIn(SD_HOST *c, PUCHAR buf, ULONG words)
{
    ULONG i;
#ifdef SD_DATA_PORT
    volatile ULONG *port = SD_DATA_PORT(c);
    if (((ULONG_PTR)buf & 3) == 0) {
        ULONG *d = (ULONG *)buf;
        for (i = 0; i < words; i++) d[i] = *port;
        return;
    }
    for (i = 0; i < words; i++) {
        ULONG w = *port;
        buf[0] = (UCHAR)w; buf[1] = (UCHAR)(w >> 8); buf[2] = (UCHAR)(w >> 16); buf[3] = (UCHAR)(w >> 24);
        buf += 4;
    }
#else
    for (i = 0; i < words; i++) {
        ULONG w = SD_RD32(c, SDHC_BUFFER);
        buf[0] = (UCHAR)w; buf[1] = (UCHAR)(w >> 8); buf[2] = (UCHAR)(w >> 16); buf[3] = (UCHAR)(w >> 24);
        buf += 4;
    }
#endif
}

static VOID SdFifoOut(SD_HOST *c, const UCHAR *buf, ULONG words)
{
    ULONG i;
#ifdef SD_DATA_PORT
    volatile ULONG *port = SD_DATA_PORT(c);
    if (((ULONG_PTR)buf & 3) == 0) {
        const ULONG *s = (const ULONG *)buf;
        for (i = 0; i < words; i++) *port = s[i];
        return;
    }
#endif
    for (i = 0; i < words; i++) {
        ULONG w = (ULONG)buf[0] | ((ULONG)buf[1] << 8) | ((ULONG)buf[2] << 16) | ((ULONG)buf[3] << 24);
#ifdef SD_DATA_PORT
        *port = w;
#else
        SD_WR32(c, SDHC_BUFFER, w);
#endif
        buf += 4;
    }
}

/* PIO data phase of the command just issued */
static SD_STATUS SdPio(SD_HOST *c, BOOLEAN read, PUCHAR buf, ULONG blocks, ULONG blksz)
{
    ULONG b, s, words = blksz / 4;
    ULONG need = read ? INT_BRR : INT_BWR;

    for (b = 0; b < blocks; b++) {
        s = SdWaitInt(c, need, 1000000);
        if (!(s & need) || (s & INT_ERRMASK)) { c->LastIntSts = s; return s ? SD_EDATA : SD_ETIMEOUT; }
        SD_WR32(c, SDHC_INTSTS, need);
        if (read) SdFifoIn(c, buf, words); else SdFifoOut(c, buf, words);
        buf += words * 4;
    }
    s = SdWaitInt(c, INT_TC, 2000000);              /* writes: includes card programming busy */
    c->LastIntSts = s;
    if (!(s & INT_TC) || (s & INT_ERRMASK)) return s ? SD_EDATA : SD_ETIMEOUT;
    SD_WR32(c, SDHC_INTSTS, INT_TC);
    return SD_OK;
}

/* CMD6 high-speed switch; returns TRUE if the card is now in high-speed timing */
static BOOLEAN SdSwitchHighSpeed(SD_HOST *c)
{
    UCHAR st[64];
    ULONG mode;

    if (!(SdCsdBits(c->Csd, 84, 12) & (1 << 10))) return FALSE;   /* no CMD6 (class 10) */
    for (mode = 0; mode < 2; mode++) {
        SD_WR16(c, SDHC_BLKSIZE, 0x7000 | 64);
        SD_WR16(c, SDHC_BLKCOUNT, 1);
        if (SdCmd(c, 6, (mode ? 0x80000000UL : 0) | 0x00FFFFF1UL, R_R1 | CMD_DATA, XFER_READ, NULL) != SD_OK) return FALSE;
        if (SdPio(c, TRUE, st, 1, 64) != SD_OK) { SdRecover(c); return FALSE; }
        if ((st[16] & 0x0F) != 1) return FALSE;
    }
    return TRUE;
}

static ULONG SdTargetHz(SD_HOST *c)
{
    switch (c->SpeedLevel) {
    case 0:  return c->HighSpeed ? 26000000UL : 13000000UL;
    case 1:  return 13000000UL;
    default: return 6500000UL;
    }
}

/* Full card (re)initialisation. Safe to call at any time; leaves the card in transfer state. */
static SD_STATUS SdInitCard(SD_HOST *c)
{
    ULONG r[4], i, ocr = 0;
    BOOLEAN v2;
    SD_STATUS st;

    c->Ready = FALSE; c->Wide = FALSE; c->HighSpeed = FALSE; c->Rca = 0;
    c->SpecVer = SD_RD16(c, SDHC_VERSION) & 0xFF;
    if (c->BaseHz == 0) c->BaseHz = SD_DEFAULT_BASE;

    if (!SdReset(c, 0x01)) return SD_ETIMEOUT;
    SD_WR32(c, SDHC_SIGEN, 0);                       /* polled driver: never raise an interrupt */
    SD_WR32(c, SDHC_INTEN, INT_ENABLE_ALL);          /* status bits only latch when enabled */
    SD_WR32(c, SDHC_INTSTS, 0xFFFFFFFF);
    SD_WR8(c, SDHC_TIMEOUT, 0x0E);
    SdHostCtrl(c);
    SD_WR8(c, SDHC_POWER, 0x0E);                     /* 3.3 V */
    SD_WR8(c, SDHC_POWER, 0x0F);                     /* bus power on (LDO4/5 were enabled by firmware) */
    for (i = 0; i < 100 && !(SD_RD32(c, SDHC_PRESENT) & PRES_CARD_STABLE); i++) SD_STALL(1000);
    if (!(SD_RD32(c, SDHC_PRESENT) & PRES_CARD_IN)) SD_LOG("goyasdhc: controller reports no card (CD override ignored?)\n");

    st = SdSetClock(c, 400000);
    if (st != SD_OK) return st;

    SdCmd(c, 0, 0, R_NONE, 0, NULL);
    SD_STALL(1000);
    v2 = (SdCmd(c, 8, 0x1AA, R_R7, 0, r) == SD_OK) && ((r[0] & 0xFFF) == 0x1AA);

    for (i = 0; i < 1000; i++) {                     /* ACMD41 for up to ~1 s */
        st = SdAppCmd(c, 41, (v2 ? 0x40000000UL : 0) | 0x00FF8000UL, R_R3, r);
        if (st == SD_OK && (r[0] & 0x80000000UL)) { ocr = r[0]; break; }
        SD_STALL(1000);
    }
    if (!(ocr & 0x80000000UL)) return SD_ENOCARD;
    c->HighCap = v2 && (ocr & 0x40000000UL);

    if ((st = SdCmd(c, 2, 0, R_R2, 0, c->Cid)) != SD_OK) return st;
    if ((st = SdCmd(c, 3, 0, R_R6, 0, r)) != SD_OK) return st;
    c->Rca = r[0] >> 16;
    if ((st = SdCmd(c, 9, c->Rca << 16, R_R2, 0, c->Csd)) != SD_OK) return st;

    if (SdCsdBits(c->Csd, 126, 2) == 1) {            /* CSD 2.0 */
        c->Blocks = ((ULONGLONG)SdCsdBits(c->Csd, 48, 22) + 1) * 1024;
    } else {                                          /* CSD 1.0 */
        ULONG csize = SdCsdBits(c->Csd, 62, 12), mult = SdCsdBits(c->Csd, 47, 3), bl = SdCsdBits(c->Csd, 80, 4);
        c->Blocks = ((ULONGLONG)(csize + 1) << (mult + 2 + bl)) / SD_BLOCK;
    }

    if ((st = SdCmd(c, 7, c->Rca << 16, R_R1B, 0, r)) != SD_OK) return st;

    if (c->SpeedLevel < 3 && SdAppCmd(c, 6, 2, R_R1, r) == SD_OK) {
        c->Wide = TRUE;
    }
    SdHostCtrl(c);
    if (!c->HighCap) SdCmd(c, 16, SD_BLOCK, R_R1, 0, r);

    if (c->SpeedLevel == 0 && SdSwitchHighSpeed(c)) c->HighSpeed = TRUE;
    SdHostCtrl(c);
    if ((st = SdSetClock(c, SdTargetHz(c))) != SD_OK) return st;
    if ((st = SdWaitTran(c)) != SD_OK) return st;

    SD_LOG("goyasdhc: card ready, %s, %u MB, %u-bit, %u kHz%s\n", c->HighCap ? "SDHC/XC" : "SDSC",
           (ULONG)(c->Blocks / 2048), c->Wide ? 4 : 1, c->ClockHz / 1000, c->HighSpeed ? " HS" : "");
    c->Ready = TRUE;
    return SD_OK;
}

static SD_STATUS SdXferOnce(SD_HOST *c, BOOLEAN read, ULONGLONG lba, ULONG count, PUCHAR buf)
{
    ULONG arg = c->HighCap ? (ULONG)lba : (ULONG)(lba * SD_BLOCK);
    USHORT xfer = read ? XFER_READ : 0;
    ULONG idx;
    SD_STATUS st;

    if (count > 1) { xfer |= XFER_MULTI | XFER_BLKCNT_EN | XFER_ACMD12; idx = read ? 18 : 25; }
    else idx = read ? 17 : 24;

    SD_WR16(c, SDHC_BLKSIZE, 0x7000 | SD_BLOCK);
    SD_WR16(c, SDHC_BLKCOUNT, (USHORT)count);
    st = SdCmd(c, idx, arg, R_R1 | CMD_DATA, xfer, NULL);
    if (st == SD_OK) st = SdPio(c, read, buf, count, SD_BLOCK);
    if (st != SD_OK) {
        c->FailIntSts = c->LastIntSts;
        SdRecover(c);
        if (count > 1) SdCmd(c, 12, 0, R_R1B | CMD_ABORT, 0, NULL);
        SdWaitTran(c);
    }
    return st;
}

/* ---- asynchronous transfer: begin, then poll until done ------------------------------------------
   For a caller that cannot afford to spin while the card is busy (goyasdhc runs at DISPATCH_LEVEL):
   SdXferPoll copies blocks as long as the card delivers or accepts them and returns SD_PENDING when
   nothing has happened for IdleUs microseconds (typically while the card programs written data); the
   caller polls again later. No retries here: on an error the transfer is cleaned up and the caller
   repeats the request with SdTransfer (retries, slower clock, re-initialisation). */

static SD_STATUS SdXferFail(SD_HOST *c, SD_STATUS st)
{
    c->FailIntSts = c->LastIntSts;
    SdRecover(c);
    if (c->XCount > 1) SdCmd(c, 12, 0, R_R1B | CMD_ABORT, 0, NULL);
    SdWaitTran(c);
    c->XPhase = 0;
    return st;
}

static SD_STATUS SdXferBegin(SD_HOST *c, BOOLEAN read, ULONGLONG lba, ULONG count, PUCHAR buf)
{
    ULONG arg = c->HighCap ? (ULONG)lba : (ULONG)(lba * SD_BLOCK);
    USHORT xfer = read ? XFER_READ : 0;
    ULONG idx;
    SD_STATUS st;

    if (!c->Ready || count == 0 || count > 0xFFFF || lba + count > c->Blocks) return SD_EPARAM;
    if (count > 1) { xfer |= XFER_MULTI | XFER_BLKCNT_EN | XFER_ACMD12; idx = read ? 18 : 25; }
    else idx = read ? 17 : 24;
    SD_WR16(c, SDHC_BLKSIZE, 0x7000 | SD_BLOCK);
    SD_WR16(c, SDHC_BLKCOUNT, (USHORT)count);
    c->XCount = count;
    st = SdCmd(c, idx, arg, R_R1 | CMD_DATA, xfer, NULL);
    if (st != SD_OK) return SdXferFail(c, st);
    c->XBuf = buf; c->XDone = 0; c->XRead = read; c->XPhase = 1;
    return SD_OK;
}

static SD_STATUS SdXferPoll(SD_HOST *c, ULONG IdleUs)
{
    ULONG need = c->XRead ? INT_BRR : INT_BWR, idle = IdleUs, s;

    if (c->XPhase == 0) return SD_EPARAM;
    for (;;) {
        s = SD_RD32(c, SDHC_INTSTS);
        if (s & (INT_ERRSUM | INT_ERRMASK)) { c->LastIntSts = s; return SdXferFail(c, SD_EDATA); }
        if (c->XPhase == 1 && (s & need)) {
            SD_WR32(c, SDHC_INTSTS, need);
            if (c->XRead) SdFifoIn(c, c->XBuf, SD_BLOCK / 4); else SdFifoOut(c, c->XBuf, SD_BLOCK / 4);
            c->XBuf += SD_BLOCK;
            if (++c->XDone == c->XCount) c->XPhase = 2;
            idle = IdleUs;                                /* progress: the idle budget starts again */
            continue;
        }
        if (c->XPhase == 2 && (s & INT_TC)) {
            SD_WR32(c, SDHC_INTSTS, INT_TC);
            c->LastIntSts = s;
            c->XPhase = 0;
            return SD_OK;
        }
        if (idle == 0) return SD_PENDING;
        SD_STALL(5); idle = idle > 5 ? idle - 5 : 0;
    }
}

/* Give up on a pending asynchronous transfer (timeout or reset). */
static SD_STATUS SdXferAbort(SD_HOST *c)
{
    if (c->XPhase == 0) return SD_ETIMEOUT;
    c->LastIntSts = SD_RD32(c, SDHC_INTSTS);
    return SdXferFail(c, SD_ETIMEOUT);
}

/* Read/write with retries; on repeated failure drop speed and reinitialise the card. */
static SD_STATUS SdTransfer(SD_HOST *c, BOOLEAN read, ULONGLONG lba, ULONG count, PUCHAR buf)
{
    ULONG attempt, i, initfail = 0;
    SD_STATUS st = SD_EPARAM;

    if (count == 0 || count > 0xFFFF || lba + count > c->Blocks) return SD_EPARAM;
    for (attempt = 0; attempt < 6; attempt++) {
        if (!c->Ready && (st = SdInitCard(c)) != SD_OK) {
            c->FailIntSts = c->LastIntSts;
            if (++initfail >= 2) break;               /* the caller may run at DISPATCH_LEVEL */
            if (c->SpeedLevel < 3) c->SpeedLevel++;
            continue;
        }
        if (attempt < 2) {
            st = SdXferOnce(c, read, lba, count, buf);
        } else {                                      /* block by block */
            for (i = 0, st = SD_OK; i < count && st == SD_OK; i++) {
                ULONG r;
                for (r = 0; r < 3; r++) {
                    st = SdXferOnce(c, read, lba + i, 1, buf + i * SD_BLOCK);
                    if (st == SD_OK) break;
                    c->ErrorCount++;
                }
            }
        }
        if (st == SD_OK) return SD_OK;
        c->ErrorCount++;
        SD_LOG("goyasdhc: %s lba %u x%u failed (%d, INTSTS %08x), attempt %u\n", read ? "read" : "write",
               (ULONG)lba, count, st, c->FailIntSts, attempt);
        if (attempt >= 1) {                           /* slow down and start over */
            if (c->SpeedLevel < 3) c->SpeedLevel++;
            c->Ready = FALSE;
        }
    }
    return st;
}
