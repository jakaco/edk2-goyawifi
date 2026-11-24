/* Host-side test of WinRT/drivers/goyasdhc/sdcore.c against a simulated SDHCI + SD card.
   cc -O1 -Wall -o sim sim.c && ./sim                                                      */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

typedef uint32_t ULONG; typedef uint16_t USHORT; typedef uint8_t UCHAR; typedef int BOOLEAN;
typedef uint64_t ULONGLONG; typedef void *PVOID; typedef uint8_t *PUCHAR;
#define TRUE 1
#define FALSE 0
#define VOID void

/* ---------------- simulated hardware ---------------- */
static uint8_t R[0x100];
static uint64_t now_us;
static int opt_v3 = 1, opt_sdhc = 1, opt_require_cd = 1, opt_crc_every = 0, opt_no_cmd6 = 0;
static unsigned opt_busy_us = 0;             /* card programming time after the last written block */
static unsigned opt_drop_tc = 0;             /* every Nth write: transfer complete never raised (PXA986) */
static unsigned tc_counter;
static uint64_t busy_until;                  /* TC is raised at this time (0: none pending) */
static uint64_t dat_busy_until;              /* the card holds DAT0 low until this time */
static unsigned crc_counter;

#define CARD_BLOCKS (32768 * 2)              /* 32 MiB card */
static uint8_t *disk;
enum { ST_IDLE, ST_READY, ST_IDENT, ST_STBY, ST_TRAN, ST_DATA, ST_RCV, ST_PRG };
static int cstate, app, acmd41_count, rca = 0x1234, buswidth = 1, blklen = 512, hs;
static uint8_t fifo[512]; static int fifo_pos, fifo_len, blocks_left, xfer_read, cur_addr, xfer_multi;

static uint32_t rd32raw(int o) { return R[o] | R[o+1] << 8 | R[o+2] << 16 | (uint32_t)R[o+3] << 24; }
static void wr32raw(int o, uint32_t v) { R[o] = v; R[o+1] = v >> 8; R[o+2] = v >> 16; R[o+3] = v >> 24; }
static uint16_t rd16raw(int o) { return R[o] | R[o+1] << 8; }
static void wr16raw(int o, uint16_t v) { R[o] = v; R[o+1] = v >> 8; }

static void raise(uint32_t bits)
{
    uint32_t en = rd32raw(0x34), st = rd32raw(0x30) | (bits & en);
    if (st & 0xFFFF0000) st |= 0x8000;
    wr32raw(0x30, st);
}

static uint32_t cardstatus(void) { return (uint32_t)(cstate == ST_DATA || cstate == ST_RCV ? ST_TRAN : cstate) << 9 | 0x100 | (app ? 0x20 : 0); }

static void load_block(void)
{
    if (opt_crc_every && ++crc_counter % opt_crc_every == 0) { raise(0x00200000); if (!xfer_multi) cstate = ST_TRAN; return; }   /* data CRC */
    memcpy(fifo, disk + (size_t)cur_addr * 512, 512); fifo_pos = 0; fifo_len = 512; raise(0x20);
}

static void setcsd(void)
{
    /* build CSD (128 bits incl. CRC byte) then store bits 127:8 into RESP (SDHCI layout) */
    uint8_t csd[16] = {0};
    #define SETB(lsb, len, val) do { for (int i_ = 0; i_ < (len); i_++) if (((uint64_t)(val) >> i_) & 1) { int b_ = (lsb) + i_; csd[15 - b_ / 8] |= 1 << (b_ % 8); } } while (0)
    SETB(84, 12, opt_no_cmd6 ? 0x5B5 & ~0x400 : 0x5B5);
    if (opt_sdhc) { SETB(126, 2, 1); SETB(48, 22, CARD_BLOCKS / 1024 - 1); }
    else { SETB(126, 2, 0); SETB(80, 4, 9); SETB(47, 3, 7); SETB(62, 12, CARD_BLOCKS / 512 - 1); }
    /* SDSC: blocks = (csize+1) << (mult+2+bl) / 512 = (csize+1) * 512 * 512/512 ... (csize+1)<<(7+2+9)/512 = (csize+1)*512 */
    uint32_t r[4] = {0};
    for (int b = 8; b < 128; b++) if (csd[15 - b / 8] >> (b % 8) & 1) r[(b - 8) / 32] |= 1u << ((b - 8) % 32);
    for (int i = 0; i < 4; i++) wr32raw(0x10 + 4 * i, r[i]);
}

static void docmd(uint16_t cmd)
{
    int idx = cmd >> 8, rsp = cmd & 3, data = cmd & 0x20;
    uint32_t arg = rd32raw(0x08), xfer = rd16raw(0x0C), resp = 0;
    int ok = 1, was_app = app; app = 0;

    if (opt_require_cd && (R[0x28] & 0xC0) != 0xC0) { raise(0x00010000); return; }   /* no card -> timeout */
    if (!(rd16raw(0x2C) & 4)) { raise(0x00010000); return; }

    if (was_app) switch (idx) {
        case 41: if (cstate != ST_IDLE && cstate != ST_READY) { ok = 0; break; }
                 resp = (++acmd41_count > 3) ? 0x80FF8000u | (opt_sdhc && (arg & 0x40000000) ? 0x40000000 : 0) : 0x00FF8000;
                 if (resp & 0x80000000) { cstate = ST_READY; } goto out;
        case 6:  if (cstate != ST_TRAN) { ok = 0; break; } buswidth = (arg & 3) == 2 ? 4 : 1; resp = cardstatus(); goto out;
        default: ok = 0; goto out;
    }
    switch (idx) {
    case 0: cstate = ST_IDLE; acmd41_count = 0; buswidth = 1; hs = 0; raise(1); return;
    case 8: if (!opt_sdhc) { raise(0x00010000); return; } resp = arg & 0xFFF; break;
    case 55: resp = cardstatus() | 0x20; app = 1; break;
    case 2: if (cstate != ST_READY) { ok = 0; break; } cstate = ST_IDENT; for (int i = 0; i < 4; i++) wr32raw(0x10 + 4*i, 0x11111111u * (i+1)); raise(1); return;
    case 3: cstate = ST_STBY; resp = (uint32_t)rca << 16; break;
    case 9: if ((arg >> 16) != (uint32_t)rca) { ok = 0; break; } setcsd(); raise(1); return;
    case 7: if ((arg >> 16) != (uint32_t)rca) { ok = 0; break; } cstate = ST_TRAN; resp = cardstatus(); raise(1); raise(2); wr32raw(0x10, resp); return;
    case 13: resp = cardstatus(); break;
    case 16: blklen = arg; resp = cardstatus(); break;
    case 6:  if (opt_no_cmd6) { ok = 0; break; }
             memset(fifo, 0, 64); fifo[16] = 0x01; if (arg & 0x80000000) hs = 1;
             wr32raw(0x10, cardstatus()); raise(1); fifo_pos = 0; fifo_len = 64; blocks_left = 1; xfer_read = 1; raise(0x20); return;
    case 12: cstate = ST_TRAN; blocks_left = 0; resp = cardstatus(); wr32raw(0x10, resp); raise(1); raise(2); return;
    case 17: case 18: case 24: case 25: {
        if (cstate != ST_TRAN) { ok = 0; break; }
        int bc = rd16raw(0x06);
        cur_addr = opt_sdhc ? (int)arg : (int)(arg / 512);
        if (!opt_sdhc && arg % 512) { ok = 0; break; }
        if (cur_addr + ((idx == 18 || idx == 25) ? bc : 1) > CARD_BLOCKS) { ok = 0; break; }
        if ((rd16raw(0x04) & 0xFFF) != 512) { fprintf(stderr, "SIM: bad blksize\n"); exit(2); }
        blocks_left = (idx == 18 || idx == 25) ? bc : 1;
        if ((idx == 18 || idx == 25) && !(xfer & 0x22)) { fprintf(stderr, "SIM: multi without blkcnt\n"); exit(2); }
        xfer_read = (idx == 17 || idx == 18);
        if (!!(xfer & 0x10) != xfer_read) { fprintf(stderr, "SIM: direction mismatch\n"); exit(2); }
        xfer_multi = (idx == 18 || idx == 25);
        cstate = xfer_read ? ST_DATA : ST_RCV;
        wr32raw(0x10, cardstatus()); raise(1);
        if (xfer_read) load_block(); else { fifo_pos = 0; fifo_len = 512; raise(0x10); }
        return; }
    default: ok = 0;
    }
out:
    if (!ok) { raise(0x00010000); return; }
    (void)rsp; (void)data;
    wr32raw(0x10, resp); raise(1);
    if (rsp == 3) raise(2);
}

static void finish_block(void)
{
    blocks_left--; cur_addr++;
    if (blocks_left > 0) {
        if (xfer_read) load_block(); else { fifo_pos = 0; raise(0x10); }
    } else {
        cstate = ST_TRAN;
        if (!xfer_read && opt_drop_tc && ++tc_counter % opt_drop_tc == 0) {
            busy_until = 0;                                  /* card done, but TC is lost */
            if (opt_busy_us) dat_busy_until = now_us + opt_busy_us;
        } else if (!xfer_read && opt_busy_us) busy_until = dat_busy_until = now_us + opt_busy_us;   /* TC once programming is done */
        else raise(2);
    }
}

static uint32_t hw_rd(int o, int sz)
{
    if (o == 0x20 && sz == 4) {
        if (!xfer_read || fifo_pos >= fifo_len) { fprintf(stderr, "SIM: buffer underrun\n"); exit(2); }
        uint32_t v = fifo[fifo_pos] | fifo[fifo_pos+1] << 8 | fifo[fifo_pos+2] << 16 | (uint32_t)fifo[fifo_pos+3] << 24;
        fifo_pos += 4;
        if (fifo_pos == fifo_len) { if (fifo_len == 64) { blocks_left = 0; cstate = ST_TRAN; raise(2); } else finish_block(); }
        return v;
    }
    if (o == 0x30 && busy_until && now_us >= busy_until) { busy_until = 0; raise(2); }
    if (o == 0x24) {
        uint32_t p = 0x00F00000;                         /* DAT lines high */
        int busy = now_us < dat_busy_until;              /* card programming: DAT0 low */
        if ((R[0x28] & 0xC0) == 0xC0 || !opt_require_cd) p |= 0x00070000;
        if (busy) p &= ~0x00100000u;
        if (busy || blocks_left > 0) p |= 0x2;           /* DAT_INHIBIT: data transfer or busy phase */
        return p;
    }
    if (o == 0x2C && sz == 2) { uint16_t v = rd16raw(0x2C); if (v & 1) v |= 2; return v; }
    if (o == 0x2F) return 0;                              /* resets complete immediately */
    if (o == 0xFE && sz == 2) return opt_v3 ? 0x0002 : 0x0001;
    return sz == 1 ? R[o] : sz == 2 ? rd16raw(o) : rd32raw(o);
}

static void hw_wr(int o, int sz, uint32_t v)
{
    if (o == 0x20 && sz == 4) {
        if (xfer_read || fifo_pos >= fifo_len) { fprintf(stderr, "SIM: buffer overrun\n"); exit(2); }
        memcpy(fifo + fifo_pos, &v, 4); fifo_pos += 4;
        if (fifo_pos == 512) { memcpy(disk + (size_t)cur_addr * 512, fifo, 512); finish_block(); }
        return;
    }
    if (o == 0x30 && sz == 4) { uint32_t st = rd32raw(0x30) & ~v; if (!(st & 0xFFFF0000)) st &= ~0x8000; wr32raw(0x30, st); return; }
    if (o == 0x2F) { if (v & 1) { memset(R, 0, 0x40); } if (v & 6) { wr32raw(0x30, 0); } if (v & 5) blocks_left = 0; return; }   /* DAT reset ends the data transaction */
    if (sz == 1) R[o] = v; else if (sz == 2) wr16raw(o, v); else wr32raw(o, v);
    if (o == 0x0E && sz == 2) docmd(v);
}

/* ---------------- glue for sdcore.c ---------------- */
#define SD_RD8(c, o)      ((UCHAR)hw_rd(o, 1))
#define SD_RD16(c, o)     ((USHORT)hw_rd(o, 2))
#define SD_RD32(c, o)     hw_rd(o, 4)
#define SD_WR8(c, o, v)   hw_wr(o, 1, v)
#define SD_WR16(c, o, v)  hw_wr(o, 2, v)
#define SD_WR32(c, o, v)  hw_wr(o, 4, v)
#define SD_STALL(us)      (now_us += (us))
static int verbose = 1;
#define SD_LOG(...)       do { if (verbose) printf(__VA_ARGS__); } while (0)
#include "../../WinRT/drivers/goyasdhc/sdcore.c"

/* As goyasdhc does it: asynchronous transfer, polled with a short idle budget (each SD_PENDING = one
   timer interval of 1 ms); on an error the request is repeated with SdTransfer (retries, fallback). */
static SD_STATUS async_xfer(SD_HOST *h, BOOLEAN read, ULONGLONG lba, ULONG n, uint8_t *b, unsigned *pend)
{
    SD_STATUS st = SdXferBegin(h, read, lba, n, b);
    if (st == SD_OK) {
        while ((st = SdXferPoll(h, 500)) == SD_PENDING) {
            if (pend) (*pend)++;
            now_us += 1000;
        }
    }
    if (st != SD_OK) st = SdTransfer(h, read, lba, n, b);
    return st;
}

static int run(const char *name)
{
    SD_HOST h; memset(&h, 0, sizeof h); memset(R, 0, sizeof R);
    cstate = ST_IDLE; app = 0; acmd41_count = 0; crc_counter = 0; hs = 0; buswidth = 1;
    busy_until = dat_busy_until = 0; tc_counter = 0; blocks_left = 0;
    uint8_t *buf = malloc(128 * 512), *chk = malloc(128 * 512);
    for (int i = 0; i < CARD_BLOCKS * 512; i++) disk[i] = (uint8_t)(i * 7 + (i >> 9));
    printf("--- %s\n", name);
    if (SdInitCard(&h) != SD_OK) { printf("FAIL init\n"); return 1; }
    if (h.Blocks != CARD_BLOCKS) { printf("FAIL capacity %llu\n", (unsigned long long)h.Blocks); return 1; }
    if (!h.Wide || buswidth != 4) { printf("FAIL bus width\n"); return 1; }
    if (SdTransfer(&h, TRUE, 100, 128, buf) != SD_OK || memcmp(buf, disk + 100 * 512, 128 * 512)) { printf("FAIL read multi\n"); return 1; }
    if (SdTransfer(&h, TRUE, 7, 1, buf) != SD_OK || memcmp(buf, disk + 7 * 512, 512)) { printf("FAIL read single\n"); return 1; }
    for (int i = 0; i < 64 * 512; i++) chk[i] = (uint8_t)rand();
    if (SdTransfer(&h, FALSE, 1000, 64, chk) != SD_OK || memcmp(chk, disk + 1000 * 512, 64 * 512)) { printf("FAIL write multi\n"); return 1; }
    if (SdTransfer(&h, FALSE, CARD_BLOCKS - 1, 1, chk) != SD_OK || memcmp(chk, disk + (size_t)(CARD_BLOCKS - 1) * 512, 512)) { printf("FAIL write last\n"); return 1; }
    if (SdTransfer(&h, TRUE, CARD_BLOCKS - 1, 2, buf) != SD_EPARAM) { printf("FAIL range check\n"); return 1; }
    /* asynchronous path (goyasdhc: StartIo + timer) */
    {
        unsigned pend = 0;
        for (int i = 0; i < 128 * 512; i++) chk[i] = (uint8_t)rand();
        if (async_xfer(&h, FALSE, 2000, 128, chk, &pend) != SD_OK || memcmp(chk, disk + 2000 * 512, 128 * 512)) { printf("FAIL async write\n"); return 1; }
        if (opt_busy_us > 500 && pend == 0) { printf("FAIL async write never deferred\n"); return 1; }
        if (async_xfer(&h, TRUE, 2000, 128, buf, NULL) != SD_OK || memcmp(buf, chk, 128 * 512)) { printf("FAIL async read\n"); return 1; }
        if (async_xfer(&h, FALSE, 3000, 1, chk, NULL) != SD_OK || memcmp(chk, disk + 3000 * 512, 512)) { printf("FAIL async single\n"); return 1; }
        if (opt_busy_us > 500 && !opt_drop_tc) {       /* abort a deferred write, then the card must still work */
            if (SdXferBegin(&h, FALSE, 4000, 8, chk) != SD_OK) { printf("FAIL abort begin\n"); return 1; }
            while (SdXferPoll(&h, 500) != SD_PENDING) ;
            SdXferAbort(&h);
            if (SdTransfer(&h, TRUE, 100, 8, buf) != SD_OK || memcmp(buf, disk + 100 * 512, 8 * 512)) { printf("FAIL after abort\n"); return 1; }
        }
        printf("async ok (write deferred %u times)\n", pend);
    }
    printf("PASS (clock %u Hz, HS %d, speedlevel %u, errors %u)\n", h.ClockHz, h.HighSpeed, h.SpeedLevel, h.ErrorCount);
    return 0;
}

int main(void)
{
    int f = 0;
    disk = malloc((size_t)CARD_BLOCKS * 512);
    f |= run("SDHC, SDHCI 3.0");
    opt_v3 = 0;  f |= run("SDHC, SDHCI 2.0 divider");
    opt_v3 = 1; opt_sdhc = 0; f |= run("SDSC v1 card (no CMD8, byte addressing)");
    opt_sdhc = 1; opt_no_cmd6 = 1; f |= run("card without CMD6");
    opt_no_cmd6 = 0; opt_crc_every = 37; f |= run("data CRC error every 37 blocks (retry/fallback)");
    opt_crc_every = 0; opt_busy_us = 20000; f |= run("card busy 20 ms after writes (deferred completion)");
    opt_drop_tc = 2; f |= run("busy 20 ms, transfer complete lost on every 2nd write (finished by the data lines)");
    opt_busy_us = 0; f |= run("no busy, transfer complete lost on every 2nd write");
    opt_drop_tc = 0;
    printf(f ? "SOME TESTS FAILED\n" : "ALL TESTS PASSED\n");
    return f;
}
