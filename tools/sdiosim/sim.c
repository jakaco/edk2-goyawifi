/*
 * sdiosim - host test of WinRT/drivers/goyasdio/sdiocore.c against a model of the PXAv3 SDHCI
 * controller and an SDIO card shaped like the Marvell 88W8777 (2 functions, MANFID 02DF:9131/9132).
 *
 *   cc -O1 -Wall -o sim sim.c && ./sim
 *
 * Checks: card init (CMD5/3/7, 4-bit, high speed, CIS IDs), CMD52, block lengths, CMD53 byte and block
 * mode (count recomputed from the length), card interrupt status gating, error paths (missing card,
 * R5 error flags, data CRC error, missing transfer complete).
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

typedef uint32_t ULONG; typedef uint16_t USHORT; typedef uint8_t UCHAR; typedef int BOOLEAN;
typedef void *PVOID;
#define TRUE 1
#define FALSE 0
#define VOID void

/* ---- controller model ---- */
static uint8_t R[0x200];
static uint64_t now_us;
static int opt_nocard, opt_crc_on_next_data, opt_drop_tc_next, opt_v3 = 1;
static uint32_t rd32raw(int o) { return R[o] | R[o+1] << 8 | R[o+2] << 16 | (uint32_t)R[o+3] << 24; }
static void wr32raw(int o, uint32_t v) { R[o] = v; R[o+1] = v >> 8; R[o+2] = v >> 16; R[o+3] = v >> 24; }
static uint16_t rd16raw(int o) { return R[o] | R[o+1] << 8; }
static void wr16raw(int o, uint16_t v) { R[o] = v; R[o+1] = v >> 8; }

/* ---- card model ---- */
static uint8_t cccr[0x100], fbr[8][0x100];
static uint8_t cis[0x400];                  /* at 0x1000..0x13FF */
static uint8_t fn1_reg[0x10000];            /* function 1 register space (17-bit address, we model 64K) */
static uint8_t port_buf[4096]; static int port_len;   /* function 1 data port at 0x10000+ (fixed address) */
#define PORT_ADDR 0x00010
static int card_state;                      /* 0 idle, 1 ready (CMD5 done), 2 ident, 3 stby, 4 cmd */
static int ocr_polls, rca = 0x0001, card_int_req;
static int last_count_field, last_blkmode, last_len;

static void card_reset(void)
{
    memset(cccr, 0, sizeof cccr); memset(fbr, 0, sizeof fbr);
    cccr[0x00] = 0x32;                      /* CCCR 3.0, SDIO 2.0 */
    cccr[0x08] = 0x17;                      /* SMB SRW S4MI SBS... not LSC: 4-bit ok */
    cccr[0x09] = 0x00; cccr[0x0A] = 0x10; cccr[0x0B] = 0x00;   /* common CIS at 0x1000 */
    cccr[0x13] = 0x01;                      /* SHS */
    fbr[1][0x00] = 0x07; fbr[1][0x09] = 0x40; fbr[1][0x0A] = 0x10;   /* fn1 CIS at 0x1040 */
    fbr[2][0x00] = 0x02; fbr[2][0x09] = 0x80; fbr[2][0x0A] = 0x10;   /* fn2 CIS at 0x1080 */
    card_state = 0; ocr_polls = 0; card_int_req = 0; port_len = 0;
}

static void cis_init(void)
{
    static const uint8_t common[] = { 0x21, 0x02, 0x0C, 0x00, 0x22, 0x04, 0x00, 0x00, 0x02, 0x32,
                                      0x20, 0x04, 0xDF, 0x02, 0x31, 0x91, 0xFF };
    static const uint8_t f1[] = { 0x21, 0x02, 0x0C, 0x00, 0x22, 0x2A, 0x01, 0x01, 0x30, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                  0x00, 0x02, /* max blk 512 at body offset 12..13 */
                                  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                                  0xFF };
    static const uint8_t f2[] = { 0x20, 0x04, 0xDF, 0x02, 0x32, 0x91, 0xFF };
    memset(cis, 0xFF, sizeof cis);
    memcpy(cis + 0x00, common, sizeof common);
    memcpy(cis + 0x40, f1, sizeof f1);
    memcpy(cis + 0x80, f2, sizeof f2);
}

static int fn0_read(uint32_t a, uint8_t *v)
{
    if (a < 0x100) { *v = cccr[a]; return 0; }
    if (a < 0x800) { *v = fbr[a >> 8][a & 0xFF]; return 0; }
    if (a >= 0x1000 && a < 0x1400) { *v = cis[a - 0x1000]; return 0; }
    *v = 0; return 0;
}
static void fn0_write(uint32_t a, uint8_t v)
{
    if (a == 0x06 && (v & 0x08)) { card_reset(); return; }
    if (a == 0x02) { cccr[0x02] = v; cccr[0x03] = v & 0x06; return; }   /* functions ready at once */
    if (a == 0x04 || a == 0x07 || a == 0x10 || a == 0x11) { cccr[a] = v; return; }
    if (a == 0x13) { cccr[0x13] = (cccr[0x13] & 1) | (v & 2); return; }
    if (a >= 0x100 && a < 0x800 && ((a & 0xFF) == 0x10 || (a & 0xFF) == 0x11)) { fbr[a >> 8][a & 0xFF] = v; return; }
}
static int card_dat1_int(void)              /* DAT1 low: function 1 interrupt request with IEN */
{
    return card_int_req && (cccr[0x04] & 0x01) && (cccr[0x04] & 0x02);
}

/* ---- controller: command execution ---- */
static uint8_t fifo[2048]; static int fifo_pos, fifo_len, blocks_left, xfer_read, xfer_fn, xfer_addr, xfer_incr, blksz;
static uint32_t intsts;
static void raise(uint32_t b) { intsts |= b; }

static uint32_t r5(uint8_t data, int err) { return (err ? 0x0800 : 0) | ((card_state == 4) ? 0x2000 : 0x1000) | data; }

static void data_load_block(void)
{
    int i;
    for (i = 0; i < blksz; i++) {
        uint32_t a = xfer_addr + (xfer_incr ? i : 0);
        if (xfer_fn == 1 && a == PORT_ADDR) fifo[i] = port_len ? port_buf[(fifo_pos + i) % port_len] : 0;
        else if (xfer_fn == 1) fifo[i] = fn1_reg[a & 0xFFFF];
        else { uint8_t v; fn0_read(a, &v); fifo[i] = v; }
    }
    fifo_pos = 0; fifo_len = blksz;
    raise(0x20);                            /* BRR */
}

static void docmd(uint16_t cmd)
{
    int idx = cmd >> 8;
    uint32_t arg = rd32raw(0x08), resp = 0;
    uint16_t xfer = rd16raw(0x0C);
    int has_resp = (cmd & 3) != 0;

    if (opt_nocard) { raise(0x00018000); return; }     /* CTO */
    switch (idx) {
    case 0: card_state = card_state == 4 ? 4 : 0; break;
    case 5:
        resp = 0x20FF8000 | ((card_state >= 1 || (arg && ++ocr_polls > 2)) ? 0x80000000u : 0);
        if (arg && (resp & 0x80000000u)) card_state = 1;
        break;
    case 3: if (card_state < 1) { raise(0x00018000); return; } card_state = 3; resp = (uint32_t)rca << 16; break;
    case 7: if ((arg >> 16) != (uint32_t)rca) { raise(0x00018000); return; } card_state = 4; resp = 0x00000700; break;
    case 52: {
        int w = arg >> 31, fn = (arg >> 28) & 7, raw = (arg >> 27) & 1; uint32_t a = (arg >> 9) & 0x1FFFF; uint8_t d = arg & 0xFF, v = 0;
        if (fn > 2) { resp = 0x1000 | 0x0200; break; }     /* function number error */
        if (fn == 0) { if (w) fn0_write(a, d); fn0_read(a, &v); if (w && !raw) v = d; }
        else if (fn == 1) { if (w) fn1_reg[a & 0xFFFF] = d; v = fn1_reg[a & 0xFFFF];
                            if (w && a == 0x05) card_int_req = 0; }     /* fn1 reg 5: clear interrupt */
        resp = r5(v, 0);
        break; }
    case 53: {
        int w = arg >> 31, fn = (arg >> 28) & 7, blk = (arg >> 27) & 1, op = (arg >> 26) & 1; uint32_t a = (arg >> 9) & 0x1FFFF, cnt = arg & 0x1FF;
        int bs = rd16raw(0x04) & 0xFFF, nb = rd16raw(0x06);
        last_count_field = cnt; last_blkmode = blk;
        if (fn > 2) { resp = 0x1000 | 0x0200; break; }
        if (blk) { int fbs = fn ? (fbr[fn][0x10] | fbr[fn][0x11] << 8) : (cccr[0x10] | cccr[0x11] << 8);
                   if (bs != fbs || (int)cnt != nb || !(xfer & 0x2)) { printf("  !! CMD53 block mode mismatch: host bs %d fn bs %d cnt %u host %d\n", bs, fbs, cnt, nb); resp = r5(0, 1); break; } }
        else if ((cnt ? (int)cnt : 512) != bs) { printf("  !! CMD53 byte mode count %u host block %d\n", cnt, bs); resp = r5(0, 1); break; }
        xfer_fn = fn; xfer_addr = a; xfer_incr = op; blksz = bs; blocks_left = blk ? nb : 1; xfer_read = !w;
        last_len = blksz * blocks_left;
        if (!w && fn == 1 && a == PORT_ADDR) fifo_pos = 0;
        if (w && fn == 1 && a == PORT_ADDR) port_len = 0;
        resp = r5(0, 0);
        wr32raw(0x10, resp); raise(0x01);
        if (xfer_read) data_load_block(); else { fifo_pos = 0; fifo_len = blksz; raise(0x10); }
        return; }
    default: raise(0x00018000); return;
    }
    if (has_resp) wr32raw(0x10, resp);
    raise(0x01);
    if ((cmd & 3) == 3) raise(0x02);        /* R1b busy over at once */
}

static void block_done(void)
{
    if (opt_crc_on_next_data) { opt_crc_on_next_data = 0; raise(0x00208000); blocks_left = 0; return; }
    if (!xfer_read) {
        int i;
        for (i = 0; i < blksz; i++) {
            uint32_t a = xfer_addr + (xfer_incr ? i : 0);
            if (xfer_fn == 1 && a == PORT_ADDR) { if (port_len < (int)sizeof port_buf) port_buf[port_len++] = fifo[i]; }
            else if (xfer_fn == 1) fn1_reg[a & 0xFFFF] = fifo[i];
        }
    }
    if (--blocks_left > 0) { if (xfer_read) { int keep = fifo_pos; data_load_block(); (void)keep; } else { fifo_pos = 0; raise(0x10); } }
    else { if (opt_drop_tc_next) opt_drop_tc_next = 0; else raise(0x02); }
}

static uint32_t hw_rd(int o, int sz)
{
    uint32_t v;
    if (o == 0x20 && sz == 4) {
        v = 0;
        if (xfer_read && fifo_pos < fifo_len) {
            int i; for (i = 0; i < 4; i++) v |= (uint32_t)(fifo_pos + i < fifo_len ? fifo[fifo_pos + i] : 0) << (8 * i);
            fifo_pos += 4;
            if (fifo_pos >= fifo_len) block_done();
        }
        return v;
    }
    if (o == 0x30 && sz == 4) {
        v = intsts & ~0x100u;
        if (card_dat1_int() && (rd32raw(0x34) & 0x100) && blocks_left == 0) v |= 0x100;
        if (v & 0xFFFF0000u) v |= 0x8000;
        return v;
    }
    if (o == 0x24 && sz == 4) return 0x01F70000 | (card_dat1_int() ? 0 : 0x00200000);   /* DAT lines, card in */
    if (o == 0x2C && sz == 2) { uint16_t c = rd16raw(0x2C); return (c & 1) ? (c | 2) : c; }
    if (o == 0x2F && sz == 1) return 0;     /* resets finish at once */
    if (o == 0xFE && sz == 2) return opt_v3 ? 0x0002 : 0x0001;
    if (sz == 1) return R[o];
    if (sz == 2) return rd16raw(o);
    return rd32raw(o);
}

static void hw_wr(int o, int sz, uint32_t v)
{
    if (o == 0x30 && sz == 4) { intsts &= ~(v & ~0x100u); return; }
    if (o == 0x20 && sz == 4) {
        if (!xfer_read && fifo_pos < fifo_len) {
            int i; for (i = 0; i < 4 && fifo_pos + i < fifo_len; i++) fifo[fifo_pos + i] = (uint8_t)(v >> (8 * i));
            fifo_pos += 4;
            if (fifo_pos >= fifo_len) block_done();
        }
        return;
    }
    if (o == 0x2F && sz == 1) {
        if (v & 1) { memset(R, 0, sizeof R); intsts = 0; blocks_left = 0; }
        if (v & 4) { blocks_left = 0; fifo_pos = fifo_len = 0; }
        return;
    }
    if (o == 0x0E && sz == 2) { wr16raw(o, v); docmd(v); return; }
    if (sz == 1) R[o] = v; else if (sz == 2) wr16raw(o, v); else wr32raw(o, v);
}

#define SD_RD8(c, o)      ((UCHAR)hw_rd(o, 1))
#define SD_RD16(c, o)     ((USHORT)hw_rd(o, 2))
#define SD_RD32(c, o)     hw_rd(o, 4)
#define SD_WR8(c, o, v)   hw_wr(o, 1, v)
#define SD_WR16(c, o, v)  hw_wr(o, 2, v)
#define SD_WR32(c, o, v)  hw_wr(o, 4, v)
#define SD_STALL(us)      (now_us += (us))
static int verbose = 1;
#define SD_LOG(...)       do { if (verbose) printf(__VA_ARGS__); } while (0)
#include "../../WinRT/drivers/goyasdio/sdiocore.c"

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void fresh(SDIO_HOST *h)
{
    memset(h, 0, sizeof *h); memset(R, 0, sizeof R); intsts = 0; blocks_left = 0;
    card_reset(); cis_init();
    h->BaseHz = 52000000; h->MaxHz = 26000000; h->ClkDelay = 15;
}

static void t_init(void)
{
    SDIO_HOST h; fresh(&h);
    CHECK(SdioInitCard(&h) == SD_OK, "init failed");
    CHECK(h.NumFuncs == 2, "functions %u", h.NumFuncs);
    CHECK(h.CardVid == 0x02DF && h.CardPid == 0x9131, "card id %04x:%04x", h.CardVid, h.CardPid);
    CHECK(h.FnVid[1] == 0x02DF && h.FnPid[1] == 0x9131, "fn1 id %04x:%04x (from the common CIS)", h.FnVid[1], h.FnPid[1]);
    CHECK(h.FnPid[2] == 0x9132, "fn2 pid %04x", h.FnPid[2]);
    CHECK(h.FnMaxBlk[1] == 512 && h.FnMaxBlk[0] == 0x200, "max blk fn1 %u fn0 %u", h.FnMaxBlk[1], h.FnMaxBlk[0]);
    CHECK(h.Wide && (cccr[0x07] & 3) == 2, "4-bit: host %d card %02x", h.Wide, cccr[0x07]);
    CHECK(h.HighSpeed && (cccr[0x13] & 2), "high speed");
    CHECK(h.ClockHz == 26000000, "clock %u", h.ClockHz);
    CHECK((rd16raw(0x10A) >> 9 & 0x1F) == 15 && (rd16raw(0x10A) & 0x100), "PXA clock delay %04x", rd16raw(0x10A));
    CHECK((R[0x28] & 0xC6) == 0xC6, "host ctrl %02x", R[0x28]);
    CHECK(rd32raw(0x38) == 0, "signals must stay off");
}

static void t_cmd52_and_blocks(void)
{
    SDIO_HOST h; UCHAR v = 0; fresh(&h);
    SdioInitCard(&h);
    CHECK(SdioWrite8(&h, 1, 0x30, 0xA5) == SD_OK && fn1_reg[0x30] == 0xA5, "fn1 write");
    CHECK(SdioRead8(&h, 1, 0x30, &v) == SD_OK && v == 0xA5, "fn1 read %02x", v);
    CHECK(SdioRead8(&h, 3, 0x30, &v) == SD_ECARD, "fn3 must fail with the R5 function-number flag");
    CHECK(SdioEnableFunction(&h, 1, TRUE) == SD_OK && (cccr[0x02] & 2), "enable fn1");
    CHECK(SdioSetBlockLen(&h, 1, 256) == SD_OK && fbr[1][0x10] == 0 && fbr[1][0x11] == 1 && h.FnBlk[1] == 256, "fn1 block 256");
    CHECK(SdioSetBlockLen(&h, 0, 64) == SD_OK && cccr[0x10] == 64, "fn0 block 64");
    CHECK(SdioSetBlockLen(&h, 3, 64) == SD_EPARAM, "fn3 block length must be refused");
    /* the MLAN driver writes FBR1 0x110/0x111 with CMD52 itself: the shadow must follow */
    CHECK(SdioCmd52Arg(&h, ARG52(1, 0, 0, 0x110, 0x00), NULL) == SD_OK && SdioCmd52Arg(&h, ARG52(1, 0, 0, 0x111, 0x02), NULL) == SD_OK
          && h.FnBlk[1] == 512, "fn1 block via raw CMD52: %u", h.FnBlk[1]);
}

static void t_cmd53(void)
{
    SDIO_HOST h; UCHAR out[1024], in[1024]; ULONG r5v; int i; fresh(&h);
    SdioInitCard(&h); SdioEnableFunction(&h, 1, TRUE); SdioSetBlockLen(&h, 1, 256);
    for (i = 0; i < 1024; i++) out[i] = (UCHAR)(i * 7 + 3);

    /* block mode write, 2 blocks, fixed address (port), count field garbage like the MLAN driver */
    ULONG argw = (1UL << 31) | (1UL << 28) | (1UL << 27) | ((ULONG)PORT_ADDR << 9) | 0x100;
    CHECK(SdioCmd53(&h, argw, out, 512, 256, &r5v) == SD_OK, "block write");
    CHECK(last_blkmode == 1 && last_count_field == 2, "count field %d (want 2)", last_count_field);
    CHECK(port_len == 512 && !memcmp(port_buf, out, 512), "port got %d bytes", port_len);

    ULONG argr = (1UL << 28) | (1UL << 27) | ((ULONG)PORT_ADDR << 9);
    memset(in, 0, sizeof in);
    CHECK(SdioCmd53(&h, argr, in, 512, 256, &r5v) == SD_OK, "block read");
    CHECK(!memcmp(in, out, 512), "block read data");

    /* byte mode: 64, 3 (word tail), 512 (count 0) to incrementing registers */
    ULONG argb = (1UL << 31) | (1UL << 28) | (1UL << 26) | (0x100UL << 9);
    CHECK(SdioCmd53(&h, argb, out, 64, 256, NULL) == SD_OK && !memcmp(fn1_reg + 0x100, out, 64), "byte write 64");
    CHECK(last_blkmode == 0 && last_count_field == 64, "byte count %d", last_count_field);
    memset(in, 0, 8);
    CHECK(SdioCmd53(&h, (1UL << 28) | (1UL << 26) | (0x100UL << 9), in, 3, 256, NULL) == SD_OK, "byte read 3");
    CHECK(in[0] == out[0] && in[1] == out[1] && in[2] == out[2] && in[3] == 0, "byte read 3 data %02x %02x %02x %02x", in[0], in[1], in[2], in[3]);
    CHECK(SdioCmd53(&h, argb, out, 512, 256, NULL) == SD_OK && last_count_field == 0, "byte 512 count %d", last_count_field);
    CHECK(SdioCmd53(&h, argb, out, 513, 256, NULL) == SD_EPARAM, "byte 513 must be refused");
    CHECK(SdioCmd53(&h, argw, out, 300, 256, NULL) == SD_EPARAM, "block length not a multiple must be refused");
}

static void t_errors(void)
{
    SDIO_HOST h; UCHAR buf[512]; ULONG e0; fresh(&h);
    opt_nocard = 1;
    CHECK(SdioInitCard(&h) == SD_ENOCARD, "no card must give SD_ENOCARD");
    opt_nocard = 0;
    fresh(&h); SdioInitCard(&h); SdioEnableFunction(&h, 1, TRUE); SdioSetBlockLen(&h, 1, 256);
    e0 = h.Errors;
    opt_crc_on_next_data = 1;
    CHECK(SdioCmd53(&h, (1UL << 28) | (1UL << 27) | ((ULONG)PORT_ADDR << 9), buf, 256, 256, NULL) == SD_EDATA, "CRC error must fail");
    CHECK(h.Errors == e0 + 1, "error counted");
    CHECK(SdioWrite8(&h, 1, 0x31, 0x5A) == SD_OK, "bus usable after the error");
    opt_drop_tc_next = 1;
    CHECK(SdioCmd53(&h, (1UL << 31) | (1UL << 28) | (1UL << 27) | ((ULONG)PORT_ADDR << 9), buf, 256, 256, NULL) == SD_OK && h.TcMissing == 1,
          "missing transfer complete after the last block is accepted (TcMissing %u)", h.TcMissing);
    CHECK(SdioWrite8(&h, 1, 0x31, 0x5B) == SD_OK, "bus usable after missing TC");
}

static void t_cardint(void)
{
    SDIO_HOST h; fresh(&h);
    SdioInitCard(&h); SdioEnableFunction(&h, 1, TRUE);
    card_int_req = 1;
    CHECK(!SdioCardIntAsserted(&h), "no card interrupt before IEN");
    CHECK(SdioFnIntEnable(&h, 1, TRUE) == SD_OK && cccr[0x04] == 0x03, "IEN %02x", cccr[0x04]);
    CHECK(!SdioCardIntAsserted(&h), "status disabled: not visible");
    SdioCardIntStatus(&h, TRUE);
    CHECK(SdioCardIntAsserted(&h), "card interrupt visible");
    SdioCardIntStatus(&h, FALSE);                        /* poll thread masks it while the client works */
    CHECK(!SdioCardIntAsserted(&h), "masked");
    CHECK(SdioWrite8(&h, 1, 0x05, 0) == SD_OK, "client clears the source");
    SdioCardIntStatus(&h, TRUE);                         /* acknowledge */
    CHECK(!SdioCardIntAsserted(&h), "cleared source: no interrupt after acknowledge");
    CHECK(SdioFnIntEnable(&h, 1, FALSE) == SD_OK && cccr[0x04] == 0, "IEN off %02x", cccr[0x04]);
}

static void t_v2_clock(void)
{
    SDIO_HOST h; fresh(&h); opt_v3 = 0;
    CHECK(SdioInitCard(&h) == SD_OK && h.ClockHz == 26000000, "v2 divider clock %u", h.ClockHz);
    opt_v3 = 1;
}

int main(void)
{
    verbose = 0;
    t_init(); t_cmd52_and_blocks(); t_cmd53(); t_errors(); t_cardint(); t_v2_clock();
    verbose = 1;
    { SDIO_HOST h; fresh(&h); printf("init log:\n"); SdioInitCard(&h); }
    printf(fails ? "%d FAILED\n" : "all sdiocore tests passed\n", fails);
    return fails != 0;
}
