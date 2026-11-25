/*
 * sdiocore.c - SDIO card protocol on the PXA988 SDHCI (PXAv3) host, polled, PIO.
 *
 * Used by goyasdio.c (Windows WDM bus driver for the Marvell 88W8777 on SDH2) and by tools/sdsim
 * (Linux simulator test). The includer defines, before including this file:
 *   SD_RD8/16/32(c, off), SD_WR8/16/32(c, off, v)   register access on SDIO_HOST *c
 *   SD_STALL(us)                                     busy wait
 *   SD_LOG(fmt, ...)                                 debug print (may be empty)
 *   ULONG/USHORT/UCHAR/BOOLEAN types, TRUE/FALSE
 *
 * Only what the Marvell MLAN driver (mwls97w8arm.sys, Surface RT) asks of the Windows SD bus:
 * CMD52, CMD53 (byte and block mode), bus width/clock, block lengths, card interrupt.
 * Card init follows Linux mmc_sdio_init_card: CMD52 reset, CMD0, CMD5 (OCR), CMD3, CMD7, 4-bit,
 * high-speed timing, CIS for the vendor/device IDs.
 *
 * SPDX-License-Identifier: MIT
 */

/* ---- SDHCI registers ---- */
#define SDHC_SDMA       0x00
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
#define SDHC_PXA_CLKBURST 0x10A           /* PXAv3 SD_CLOCK_BURST_SIZE_SETUP: SDCLK delay (Linux sdhci-pxav3) */
#define   PXA_SDCLK_SEL       0x0100
#define   PXA_SDCLK_DELAY_SHIFT 9
#define   PXA_SDCLK_DELAY_MASK  0x1F

#define PRES_CMD_INHIBIT  0x00000001
#define PRES_DAT_INHIBIT  0x00000002
#define PRES_DAT0         0x00100000
#define PRES_DAT1         0x00200000

#define HC_4BIT           0x02
#define HC_HISPEED        0x04
#define HC_CD_TEST_LVL    0x40
#define HC_CD_SIG_SEL     0x80

#define INT_CC            0x00000001
#define INT_TC            0x00000002
#define INT_BWR           0x00000010
#define INT_BRR           0x00000020
#define INT_CARD          0x00000100      /* card interrupt (DAT1), level: not write-1-to-clear */
#define INT_ERRSUM        0x00008000
#define INT_ERRMASK       0xFFFF0000
#define INT_CTO           0x00010000
#define INT_DTO           0x00100000
#define INT_ENABLE_BASE   0x03FF0033      /* CC TC BWR BRR + all errors (status only, signals stay off) */

#define XFER_BLKCNT_EN    0x0002
#define XFER_READ         0x0010
#define XFER_MULTI        0x0020

#define CMD_RSP_NONE      0x0000
#define CMD_RSP_48        0x0002
#define CMD_RSP_48B       0x0003
#define CMD_CRC           0x0008
#define CMD_IDX           0x0010
#define CMD_DATA          0x0020

#define R_NONE  CMD_RSP_NONE
#define R_R1    (CMD_RSP_48 | CMD_CRC | CMD_IDX)
#define R_R1B   (CMD_RSP_48B | CMD_CRC | CMD_IDX)
#define R_R4    CMD_RSP_48                         /* no CRC, no index */
#define R_R5    (CMD_RSP_48 | CMD_CRC | CMD_IDX)
#define R_R6    R_R1

/* ---- SDIO card registers (function 0) ---- */
#define CCCR_REV        0x00
#define CCCR_IOE        0x02
#define CCCR_IOR        0x03
#define CCCR_IEN        0x04
#define CCCR_INTPEND    0x05
#define CCCR_ABORT      0x06
#define   CCCR_ABORT_RES  0x08
#define CCCR_BUSIF      0x07
#define   BUSIF_WIDTH_MASK 0x03
#define   BUSIF_WIDTH_4    0x02
#define CCCR_CAPS       0x08
#define   CAPS_LSC        0x40
#define   CAPS_4BLS       0x80
#define CCCR_CIS        0x09                       /* 3 bytes, little endian */
#define CCCR_FN0_BLK    0x10                       /* 2 bytes */
#define CCCR_SPEED      0x13
#define   SPEED_SHS       0x01
#define   SPEED_EHS       0x02
#define FBR(fn)         ((ULONG)(fn) * 0x100)
#define FBR_CODE        0x00
#define FBR_CIS         0x09
#define FBR_BLK         0x10

#define CISTPL_NULL     0x00
#define CISTPL_MANFID   0x20
#define CISTPL_FUNCE    0x22
#define CISTPL_END      0xFF

/* R5 flags (response bits 15:8) that mean the command failed: CRC error, illegal command, error,
   function number, out of range */
#define R5_ERRORS       0xCB00UL

#define SDIO_MAX_FN     7
#define SDIO_DEFAULT_BASE 52000000UL               /* APMU SDH1: pll1_416 / 8, set by the firmware */

typedef enum { SD_OK = 0, SD_ETIMEOUT, SD_ECMD, SD_EDATA, SD_ECARD, SD_ENOCARD, SD_EPARAM } SD_STATUS;

typedef struct _SDIO_HOST {
    volatile UCHAR *Regs;
    PVOID     Os;                 /* OS context for the register macros */
    ULONG     BaseHz;             /* controller base clock (the PXAv3 capability field is not used) */
    ULONG     MaxHz;              /* bus clock limit (Linux DT: 26 MHz for this slot) */
    ULONG     ClockHz;            /* bus clock now */
    ULONG     SpecVer;
    ULONG     ClkDelay;           /* PXAv3 SDCLK delay cycles (Linux DT mrvl,clk-delay-cycles = 15), 0 = off */
    BOOLEAN   Ready;
    BOOLEAN   Wide;
    BOOLEAN   HighSpeed;          /* card and host in high-speed timing */
    BOOLEAN   CardIntOn;          /* INT_CARD status enabled (poll sees the card interrupt) */
    ULONG     Rca;
    ULONG     Ocr;
    ULONG     NumFuncs;
    UCHAR     CccrRev, Caps, BusIf;
    USHORT    CardVid, CardPid;   /* common CIS MANFID */
    USHORT    FnVid[SDIO_MAX_FN + 1], FnPid[SDIO_MAX_FN + 1];   /* function CIS MANFID (0 = none) */
    UCHAR     FnCode[SDIO_MAX_FN + 1];                          /* standard function interface code */
    USHORT    FnBlk[SDIO_MAX_FN + 1];                           /* block length set (index 0 = FN0) */
    USHORT    FnMaxBlk[SDIO_MAX_FN + 1];                        /* from CIS FUNCE, 0 = unknown */
    ULONG     LastIntSts;
    ULONG     LastR5;
    ULONG     Errors;             /* failed commands/transfers */
    ULONG     TcMissing;          /* CMD53 with every block moved but no transfer complete */
    ULONG     Cmd53s;             /* CMD53 transfers done */
} SDIO_HOST;

static BOOLEAN SdioWaitClear8(SDIO_HOST *c, ULONG off, UCHAR mask, ULONG us)
{
    while (us) {
        if (!(SD_RD8(c, off) & mask)) return TRUE;
        SD_STALL(10); us = us > 10 ? us - 10 : 0;
    }
    return !(SD_RD8(c, off) & mask);
}

static BOOLEAN SdioReset(SDIO_HOST *c, UCHAR mask)
{
    SD_WR8(c, SDHC_SWRESET, mask);
    return SdioWaitClear8(c, SDHC_SWRESET, mask, 100000);
}

static VOID SdioIntEnable(SDIO_HOST *c)
{
    SD_WR32(c, SDHC_INTEN, INT_ENABLE_BASE | (c->CardIntOn ? INT_CARD : 0));
    SD_WR32(c, SDHC_SIGEN, 0);                    /* polled: no interrupt signals (line shared with SDH1/SDH3) */
}

/* Host Control 1 is cleared by a full reset. SDH2 has no card-detect pin: force "card present" with
   the card-detect test level (the SDIO module is soldered on). */
static VOID SdioHostCtrl(SDIO_HOST *c)
{
    UCHAR h = HC_CD_SIG_SEL | HC_CD_TEST_LVL;
    if (c->Wide) h |= HC_4BIT;
    if (c->HighSpeed) h |= HC_HISPEED;
    SD_WR8(c, SDHC_HOSTCTRL, h);
}

/* Linux sdhci-pxav3 pxav3_set_clock: SDCLK delay for the SDIO/eMMC slots (mrvl,clk-delay-cycles) */
static VOID SdioClkDelay(SDIO_HOST *c)
{
    USHORT v;
    if (!c->ClkDelay) return;
    v = SD_RD16(c, SDHC_PXA_CLKBURST);
    v &= (USHORT)~(PXA_SDCLK_DELAY_MASK << PXA_SDCLK_DELAY_SHIFT);
    v |= (USHORT)(((c->ClkDelay & PXA_SDCLK_DELAY_MASK) << PXA_SDCLK_DELAY_SHIFT) | PXA_SDCLK_SEL);
    SD_WR16(c, SDHC_PXA_CLKBURST, v);
}

static SD_STATUS SdioSetClock(SDIO_HOST *c, ULONG hz)
{
    ULONG div, us;
    USHORT clk;

    if (hz > c->MaxHz) hz = c->MaxHz;
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
    SdioClkDelay(c);
    SD_STALL(1000);                                /* >= 74 clocks before the next command */
    return SD_OK;
}

static ULONG SdioWaitInt(SDIO_HOST *c, ULONG mask, ULONG us)
{
    ULONG s;
    for (;;) {
        s = SD_RD32(c, SDHC_INTSTS);
        if (s & (mask | INT_ERRSUM | INT_ERRMASK)) return s;
        if (us == 0) return 0;
        SD_STALL(10); us = us > 10 ? us - 10 : 0;
    }
}

/* Clear every write-1-to-clear status bit (the card interrupt bit is a level and is not touched) */
static VOID SdioClearInts(SDIO_HOST *c)
{
    SD_WR32(c, SDHC_INTSTS, ~INT_CARD);
}

static VOID SdioRecover(SDIO_HOST *c)
{
    SdioClearInts(c);
    SdioReset(c, 0x02);
    SdioReset(c, 0x04);
}

/* Issue one command; for data commands only the command phase is completed here. */
static SD_STATUS SdioCmd(SDIO_HOST *c, ULONG idx, ULONG arg, USHORT flags, USHORT xfer, ULONG *resp)
{
    ULONG inhibit = PRES_CMD_INHIBIT, us, s;

    if ((flags & CMD_DATA) || (flags & 3) == CMD_RSP_48B) inhibit |= PRES_DAT_INHIBIT;
    for (us = 0; us < 200000 && (SD_RD32(c, SDHC_PRESENT) & inhibit); us += 10) SD_STALL(10);
    if (SD_RD32(c, SDHC_PRESENT) & inhibit) { SdioRecover(c); c->Errors++; return SD_ETIMEOUT; }

    SdioClearInts(c);
    SD_WR32(c, SDHC_ARG, arg);
    SD_WR16(c, SDHC_XFER, xfer);
    SD_WR16(c, SDHC_CMD, (USHORT)((idx << 8) | flags));

    s = SdioWaitInt(c, INT_CC, 100000);
    c->LastIntSts = s;
    if (!(s & INT_CC) || (s & INT_ERRMASK)) {
        SdioRecover(c);
        c->Errors++;
        return (s & INT_CTO) || !s ? SD_ETIMEOUT : SD_ECMD;
    }
    SD_WR32(c, SDHC_INTSTS, INT_CC);
    if (resp) *resp = SD_RD32(c, SDHC_RESP);
    if (!(flags & CMD_DATA) && (flags & 3) == CMD_RSP_48B) {
        s = SdioWaitInt(c, INT_TC, 1000000);       /* TC = end of busy */
        if (!(s & INT_TC) || (s & INT_ERRMASK)) {
            for (us = 0; us < 1000000 && !(SD_RD32(c, SDHC_PRESENT) & PRES_DAT0); us += 10) SD_STALL(10);
            SdioRecover(c);
            if (!(SD_RD32(c, SDHC_PRESENT) & PRES_DAT0)) { c->Errors++; return SD_ETIMEOUT; }
        }
        SD_WR32(c, SDHC_INTSTS, INT_TC);
    }
    return SD_OK;
}

/* ---- CMD52 / CMD53 ---- */

#define ARG52(w, fn, raw, addr, data) \
    (((ULONG)(w) << 31) | ((ULONG)(fn) << 28) | ((ULONG)(raw) << 27) | (((ULONG)(addr) & 0x1FFFF) << 9) | ((ULONG)(data) & 0xFF))

/* Raw CMD52 with a caller-built argument; the whole R5 goes to *r5 */
static SD_STATUS SdioCmd52Arg(SDIO_HOST *c, ULONG arg, ULONG *r5)
{
    ULONG r = 0;
    SD_STATUS st = SdioCmd(c, 52, arg, R_R5, 0, &r);
    if (st != SD_OK) return st;
    c->LastR5 = r;
    if (r5) *r5 = r;
    if (r & R5_ERRORS) { c->Errors++; return SD_ECARD; }
    if ((arg >> 31) && ((arg >> 28) & 7) == 0) {
        /* function driver wrote a block length register itself (the MLAN driver writes FBR1 0x110/0x111
           before it sets SDP_FUNCTION_BLOCK_LENGTH): keep the length used for CMD53 block mode in step */
        ULONG a = (arg >> 9) & 0x1FFFF, fn = a >> 8, d = (arg & 0x08000000UL) ? (r & 0xFF) : (arg & 0xFF);
        if (a < 0x800 && ((a & 0xFF) == 0x10 || (a & 0xFF) == 0x11) && fn <= SDIO_MAX_FN) {
            if ((a & 0xFF) == 0x10) c->FnBlk[fn] = (USHORT)((c->FnBlk[fn] & 0xFF00) | d);
            else c->FnBlk[fn] = (USHORT)((c->FnBlk[fn] & 0x00FF) | (d << 8));
        }
    }
    return SD_OK;
}

static SD_STATUS SdioRead8(SDIO_HOST *c, ULONG fn, ULONG addr, UCHAR *v)
{
    ULONG r5;
    SD_STATUS st = SdioCmd52Arg(c, ARG52(0, fn, 0, addr, 0), &r5);
    if (st == SD_OK && v) *v = (UCHAR)r5;
    return st;
}

static SD_STATUS SdioWrite8(SDIO_HOST *c, ULONG fn, ULONG addr, UCHAR v)
{
    return SdioCmd52Arg(c, ARG52(1, fn, 0, addr, v), NULL);
}

/* One block through the buffer data port */
static VOID SdioFifoIn(SDIO_HOST *c, UCHAR *buf, ULONG bytes)
{
    ULONG i, w;
    for (i = 0; i + 4 <= bytes; i += 4) {
        w = SD_RD32(c, SDHC_BUFFER);
        buf[i] = (UCHAR)w; buf[i + 1] = (UCHAR)(w >> 8); buf[i + 2] = (UCHAR)(w >> 16); buf[i + 3] = (UCHAR)(w >> 24);
    }
    if (i < bytes) {                               /* byte-mode tail: the last word holds 1..3 bytes */
        w = SD_RD32(c, SDHC_BUFFER);
        for (; i < bytes; i++, w >>= 8) buf[i] = (UCHAR)w;
    }
}

static VOID SdioFifoOut(SDIO_HOST *c, const UCHAR *buf, ULONG bytes)
{
    ULONG i, w, k;
    for (i = 0; i + 4 <= bytes; i += 4)
        SD_WR32(c, SDHC_BUFFER, (ULONG)buf[i] | ((ULONG)buf[i + 1] << 8) | ((ULONG)buf[i + 2] << 16) | ((ULONG)buf[i + 3] << 24));
    if (i < bytes) {
        for (w = 0, k = 0; i < bytes; i++, k += 8) w |= (ULONG)buf[i] << k;
        SD_WR32(c, SDHC_BUFFER, w);
    }
}

/*
 * CMD53. arg is the caller's argument (R/W, function, block mode, op code, address); the count field is
 * recomputed here from len like the Windows SD bus does (the MLAN driver leaves it inconsistent).
 * Block mode: len must be a multiple of blksz (the function's block length); byte mode: 1..512 bytes.
 */
static SD_STATUS SdioCmd53(SDIO_HOST *c, ULONG arg, UCHAR *buf, ULONG len, ULONG blksz, ULONG *r5)
{
    BOOLEAN wr = (arg >> 31) & 1, blk = (arg >> 27) & 1;
    ULONG nblk, bsz, b, s, r = 0;
    USHORT xfer;
    SD_STATUS st;

    if (len == 0) return SD_EPARAM;
    if (blk) {
        if (blksz == 0 || blksz > 2048 || len % blksz) return SD_EPARAM;
        nblk = len / blksz; bsz = blksz;
        if (nblk > 511) return SD_EPARAM;
        arg = (arg & ~0x1FFUL) | nblk;
    } else {
        if (len > 512) return SD_EPARAM;
        nblk = 1; bsz = len;
        arg = (arg & ~0x1FFUL) | (len & 0x1FF);    /* 512 is encoded as 0 */
    }
    SD_WR16(c, SDHC_BLKSIZE, (USHORT)(bsz & 0xFFF));
    SD_WR16(c, SDHC_BLKCOUNT, (USHORT)nblk);
    xfer = (USHORT)((wr ? 0 : XFER_READ) | (blk ? (XFER_BLKCNT_EN | (nblk > 1 ? XFER_MULTI : 0)) : 0));
    st = SdioCmd(c, 53, arg, R_R5 | CMD_DATA, xfer, &r);
    if (st != SD_OK) return st;
    c->LastR5 = r;
    if (r5) *r5 = r;
    if (r & R5_ERRORS) { SdioRecover(c); c->Errors++; return SD_ECARD; }

    for (b = 0; b < nblk; b++) {
        ULONG need = wr ? INT_BWR : INT_BRR;
        s = SdioWaitInt(c, need, 1000000);
        if (!(s & need) || (s & INT_ERRMASK)) {
            c->LastIntSts = s; SdioRecover(c); c->Errors++;
            return s ? SD_EDATA : SD_ETIMEOUT;
        }
        SD_WR32(c, SDHC_INTSTS, need);
        if (!wr && bsz < 512 && c->ClockHz) {
            /* goyasdhc runs 10/11: on this controller the first words of a short block read at
               buffer-read-ready were old buffer contents. Give the block its bus time to land. */
            SD_STALL((bsz * 2) / (c->ClockHz / 1000000 ? c->ClockHz / 1000000 : 1) + 5);
        }
        if (wr) SdioFifoOut(c, buf, bsz); else SdioFifoIn(c, buf, bsz);
        buf += bsz;
    }
    s = SdioWaitInt(c, INT_TC, 500000);
    c->LastIntSts = s;
    if (s & INT_ERRMASK) { SdioRecover(c); c->Errors++; return SD_EDATA; }
    if (s & INT_TC) SD_WR32(c, SDHC_INTSTS, INT_TC);
    else {
        /* every block went through the buffer but transfer complete never came (seen on this
           controller for SD writes): end the transaction with a data-line reset */
        c->TcMissing++;
        SdioReset(c, 0x04);
    }
    c->Cmd53s++;
    return SD_OK;
}

/* ---- CIS ---- */

static ULONG SdioCisPtr(SDIO_HOST *c, ULONG base)
{
    UCHAR b0 = 0, b1 = 0, b2 = 0;
    if (SdioRead8(c, 0, base, &b0) || SdioRead8(c, 0, base + 1, &b1) || SdioRead8(c, 0, base + 2, &b2)) return 0;
    return (ULONG)b0 | ((ULONG)b1 << 8) | ((ULONG)b2 << 16);
}

/* Walk one CIS chain (function 0 space): MANFID gives vid/pid, FUNCE the maximum block length */
static VOID SdioParseCis(SDIO_HOST *c, ULONG ptr, BOOLEAN common, USHORT *vid, USHORT *pid, USHORT *maxblk)
{
    ULONG guard;
    UCHAR code, len, b[16];
    ULONG i;

    if (ptr < 0x1000 || ptr > 0x17FFF) return;     /* CIS area of function 0 address space */
    for (guard = 0; guard < 64; guard++) {
        if (SdioRead8(c, 0, ptr, &code)) return;
        if (code == CISTPL_END) return;
        if (code == CISTPL_NULL) { ptr++; continue; }
        if (SdioRead8(c, 0, ptr + 1, &len)) return;
        if (len == 0xFF) return;
        for (i = 0; i < len && i < sizeof(b); i++)
            if (SdioRead8(c, 0, ptr + 2 + i, &b[i])) return;
        if (code == CISTPL_MANFID && len >= 4) {
            *vid = (USHORT)(b[0] | (b[1] << 8));
            *pid = (USHORT)(b[2] | (b[3] << 8));
        } else if (code == CISTPL_FUNCE && maxblk) {
            if (common && len >= 3 && b[0] == 0) *maxblk = (USHORT)(b[1] | (b[2] << 8));          /* FN0 */
            else if (!common && len >= 14 && b[0] == 1) *maxblk = (USHORT)(b[12] | (b[13] << 8)); /* FN1-7 */
        }
        ptr += 2 + len;
    }
}

/* ---- card init ---- */

static SD_STATUS SdioSetBusWidth(SDIO_HOST *c, BOOLEAN wide)
{
    UCHAR v;
    SD_STATUS st = SdioRead8(c, 0, CCCR_BUSIF, &v);
    if (st) return st;
    v = (UCHAR)((v & ~BUSIF_WIDTH_MASK) | (wide ? BUSIF_WIDTH_4 : 0));
    st = SdioWrite8(c, 0, CCCR_BUSIF, v);
    if (st) return st;
    c->BusIf = v;
    c->Wide = wide;
    SdioHostCtrl(c);
    return SD_OK;
}

static SD_STATUS SdioSetBlockLen(SDIO_HOST *c, ULONG fn, ULONG len)
{
    SD_STATUS st;
    ULONG base = fn ? FBR(fn) + FBR_BLK : CCCR_FN0_BLK;
    if (fn > c->NumFuncs || len == 0 || len > 2048) return SD_EPARAM;
    st = SdioWrite8(c, 0, base, (UCHAR)len);
    if (st == SD_OK) st = SdioWrite8(c, 0, base + 1, (UCHAR)(len >> 8));
    if (st == SD_OK) c->FnBlk[fn] = (USHORT)len;
    return st;
}

static SD_STATUS SdioEnableFunction(SDIO_HOST *c, ULONG fn, BOOLEAN on)
{
    UCHAR v, r = 0;
    ULONG i;
    SD_STATUS st = SdioRead8(c, 0, CCCR_IOE, &v);
    if (st) return st;
    v = on ? (UCHAR)(v | (1 << fn)) : (UCHAR)(v & ~(1 << fn));
    if ((st = SdioWrite8(c, 0, CCCR_IOE, v)) != SD_OK) return st;
    if (!on) return SD_OK;
    for (i = 0; i < 1000; i++) {                   /* I/O ready: up to 1 s */
        if (SdioRead8(c, 0, CCCR_IOR, &r) == SD_OK && (r & (1 << fn))) return SD_OK;
        SD_STALL(1000);
    }
    return SD_ETIMEOUT;
}

/* CCCR interrupt enable: function bit plus the master enable while any function bit is set */
static SD_STATUS SdioFnIntEnable(SDIO_HOST *c, ULONG fn, BOOLEAN on)
{
    UCHAR v;
    SD_STATUS st = SdioRead8(c, 0, CCCR_IEN, &v);
    if (st) return st;
    v = on ? (UCHAR)(v | (1 << fn)) : (UCHAR)(v & ~(1 << fn));
    if (v & 0xFE) v |= 1; else v = 0;
    return SdioWrite8(c, 0, CCCR_IEN, v);
}

/* Card interrupt status as seen by the host (DAT1, 4-bit interrupt period) */
static BOOLEAN SdioCardIntAsserted(SDIO_HOST *c)
{
    return (SD_RD32(c, SDHC_INTSTS) & INT_CARD) != 0;
}

static VOID SdioCardIntStatus(SDIO_HOST *c, BOOLEAN on)
{
    c->CardIntOn = on;
    SD_WR32(c, SDHC_INTEN, INT_ENABLE_BASE | (on ? INT_CARD : 0));
}

static SD_STATUS SdioInitCard(SDIO_HOST *c)
{
    ULONG r = 0, i, fn;
    UCHAR v;
    SD_STATUS st;

    c->Ready = FALSE; c->Wide = FALSE; c->HighSpeed = FALSE; c->CardIntOn = FALSE;
    c->NumFuncs = 0; c->Rca = 0;
    for (fn = 0; fn <= SDIO_MAX_FN; fn++) { c->FnVid[fn] = c->FnPid[fn] = 0; c->FnCode[fn] = 0; c->FnBlk[fn] = 0; c->FnMaxBlk[fn] = 0; }
    c->CardVid = c->CardPid = 0;
    if (!c->BaseHz) c->BaseHz = SDIO_DEFAULT_BASE;
    if (!c->MaxHz) c->MaxHz = 26000000;

    if (!SdioReset(c, 0x01)) return SD_ETIMEOUT;
    c->SpecVer = SD_RD16(c, SDHC_VERSION) & 0xFF;
    SD_WR8(c, SDHC_POWER, 0x0F);                   /* 3.3 V, bus power on */
    SD_STALL(10000);
    SD_WR8(c, SDHC_TIMEOUT, 0x0E);
    SdioIntEnable(c);
    SdioHostCtrl(c);
    if (SdioSetClock(c, 400000) != SD_OK) return SD_ETIMEOUT;

    /* Linux mmc_sdio_init_card order: I/O reset (CMD52 RES, no answer expected after it), CMD0 */
    (VOID)SdioCmd(c, 52, ARG52(1, 0, 0, CCCR_ABORT, CCCR_ABORT_RES), R_R5, 0, NULL);
    SD_STALL(1000);
    (VOID)SdioCmd(c, 0, 0, R_NONE, 0, NULL);
    SD_STALL(1000);

    st = SdioCmd(c, 5, 0, R_R4, 0, &r);
    if (st != SD_OK) {
        SD_STALL(10000);
        st = SdioCmd(c, 5, 0, R_R4, 0, &r);
        if (st != SD_OK) { SD_LOG("goyasdio: no answer to CMD5 (%d)\n", st); return SD_ENOCARD; }
    }
    c->NumFuncs = (r >> 28) & 7;
    c->Ocr = r & 0x00FFFFFF;
    if (c->NumFuncs == 0 || !(c->Ocr & 0x00FF8000)) { SD_LOG("goyasdio: CMD5 R4 %08x: no I/O functions\n", r); return SD_ECARD; }
    for (i = 0; i < 100; i++) {                    /* power-up: C bit (31) within 1 s */
        st = SdioCmd(c, 5, c->Ocr & 0x00FF8000, R_R4, 0, &r);
        if (st == SD_OK && (r & 0x80000000UL)) break;
        SD_STALL(10000);
    }
    if (!(r & 0x80000000UL)) { SD_LOG("goyasdio: card stays busy, R4 %08x\n", r); return SD_ETIMEOUT; }

    if ((st = SdioCmd(c, 3, 0, R_R6, 0, &r)) != SD_OK) return st;
    c->Rca = r >> 16;
    if ((st = SdioCmd(c, 7, c->Rca << 16, R_R1B, 0, &r)) != SD_OK) return st;

    if ((st = SdioRead8(c, 0, CCCR_REV, &c->CccrRev)) != SD_OK) return st;
    if ((st = SdioRead8(c, 0, CCCR_CAPS, &c->Caps)) != SD_OK) return st;

    if (!(c->Caps & CAPS_LSC) || (c->Caps & CAPS_4BLS)) {
        if ((st = SdioSetBusWidth(c, TRUE)) != SD_OK) return st;
    } else {
        (VOID)SdioRead8(c, 0, CCCR_BUSIF, &c->BusIf);
    }

    /* high-speed timing if the card has it (Linux sdio_enable_hs), then the bus clock */
    if (SdioRead8(c, 0, CCCR_SPEED, &v) == SD_OK && (v & SPEED_SHS) && c->MaxHz > 25000000) {
        if (SdioWrite8(c, 0, CCCR_SPEED, (UCHAR)(v | SPEED_EHS)) == SD_OK) {
            c->HighSpeed = TRUE;
            SdioHostCtrl(c);
        }
    }
    if ((st = SdioSetClock(c, c->HighSpeed ? c->MaxHz : 25000000)) != SD_OK) return st;

    SdioParseCis(c, SdioCisPtr(c, CCCR_CIS), TRUE, &c->CardVid, &c->CardPid, &c->FnMaxBlk[0]);
    for (fn = 1; fn <= c->NumFuncs; fn++) {
        if (SdioRead8(c, 0, FBR(fn) + FBR_CODE, &v) == SD_OK) c->FnCode[fn] = (UCHAR)(v & 0x0F);
        SdioParseCis(c, SdioCisPtr(c, FBR(fn) + FBR_CIS), FALSE, &c->FnVid[fn], &c->FnPid[fn], &c->FnMaxBlk[fn]);
        if (!c->FnVid[fn]) { c->FnVid[fn] = c->CardVid; c->FnPid[fn] = c->CardPid; }
    }
    SD_LOG("goyasdio: card %04x:%04x, %u functions, CCCR rev %02x caps %02x, %u Hz %s%s\n",
           c->CardVid, c->CardPid, c->NumFuncs, c->CccrRev, c->Caps, c->ClockHz,
           c->Wide ? "4-bit" : "1-bit", c->HighSpeed ? " HS" : "");
    for (fn = 1; fn <= c->NumFuncs; fn++)
        SD_LOG("goyasdio:   function %u: %04x:%04x code %u max block %u\n", fn, c->FnVid[fn], c->FnPid[fn], c->FnCode[fn], c->FnMaxBlk[fn]);
    c->Ready = TRUE;
    return SD_OK;
}
